/*
 * wine_int3: a prototype kernel driver that lets Wine processes survive an int3 (a Windows DebugBreak(), or Chromium's
 * IMMEDIATE_CRASH and CHECK() failures, the Firefox sandbox, ...) the way they do on Linux, without a debugger.
 *
 * Haiku's kernel does not turn an int3 into SIGTRAP. x86_handle_breakpoint_exception() backs the instruction pointer up onto
 * the int3 and calls user_debug_breakpoint_hit(), which reports the breakpoint to the team's debugger, or, when the team has
 * none, starts debug_server and the team is killed. Wine used to be the debugger of every Wine process for this (the "guard" in
 * wineserver), and that is a bad thing to keep installed: with a debugger installed the kernel's return from a signal handler
 * (interrupts.S, .Lpost_syscall_work_done) clears the vector registers instead of restoring them (every XMM register is zero
 * after each signal), and every system call of the thread takes the slow path.
 *
 * This driver patches the one jump in x86_handle_breakpoint_exception() that goes to user_debug_breakpoint_hit() so that it goes
 * through a function of ours. For a team that has asked for it (by opening the device and sending WINE_INT3_MARK; the mark ends
 * when the device is closed, which the kernel does when the process ends) the function puts the instruction pointer after the
 * int3 and sends the thread SIGTRAP, which is what a Linux kernel does and what Wine's SIGTRAP handler expects. For any other
 * team it calls user_debug_breakpoint_hit() as the kernel would have.
 *
 * Like wine_compat, it checks byte for byte that the code is the one it was written for (the jump is found in the body of
 * x86_handle_breakpoint_exception(), it is the only one that goes to user_debug_breakpoint_hit(), and it is preceded by
 * "mov $1, %edi"), does nothing if it is not, and patches in memory only: unloading the driver or rebooting puts the original
 * back. It patches only when the first team asks, and WINE_INT3_PROBE tells what it would patch without patching.
 */
#include <Drivers.h>
#include <KernelExport.h>
#include <OS.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

#define DEVICE_NAME "misc/wine_int3"

extern "C" void x86_handle_breakpoint_exception(void* frame);
extern "C" void user_debug_breakpoint_hit(bool software);
extern "C" int32 team_get_current_team_id(void);

struct iframe {
	uint64 type;
	void* fpu;
	uint64 r15, r14, r13, r12, r11, r10, r9, r8, bp, si, di, dx, cx, bx, ax;
	uint64 orig_rax, vector, error_code;
	uint64 ip, cs, flags, user_sp, user_ss;
};
extern "C" iframe* x86_get_current_iframe(void);

struct wine_int3_status {
	uint32 version;
	uint32 patched;			// 1 when the jump goes through the driver
	int32 handled;			// breakpoints of marked teams turned into SIGTRAP
	int32 passed;			// breakpoints of other teams, given to the kernel
	int32 marked_teams;
};
struct wine_int3_probe {
	int32 found;			// jumps to user_debug_breakpoint_hit found in the function (must be 1)
	int32 prefix_ok;		// 1 when "mov $1, %edi" comes right before it
	int32 reachable;		// 1 when the new jump can reach the driver
	int32 offset;			// the place of the jump in x86_handle_breakpoint_exception()
};
#define WINE_INT3_STATUS	(B_DEVICE_OP_CODES_END + 1)
#define WINE_INT3_PROBE		(B_DEVICE_OP_CODES_END + 2)
#define WINE_INT3_MARK		(B_DEVICE_OP_CODES_END + 3)

int32 api_version = B_CUR_DRIVER_API_VERSION;

#define MAX_TEAMS 128
#define SCAN_BYTES 96			// x86_handle_breakpoint_exception() is about 48 bytes long

static spinlock sLock = B_SPINLOCK_INITIALIZER;
static int32 sTeams[MAX_TEAMS];
static int32 sTeamCount = 0;
static int32 sHandled = 0, sPassed = 0;
static bool sPatched = false;
static area_id sPatchArea = -1;
static volatile unsigned char* sPatchAddress = NULL;		// the operand of the jump, through our own mapping
static int32 sOriginalOperand = 0;

struct open_cookie {
	int32 team;		// the team this open file marked, or -1
};


// #pragma mark - teams


static bool
team_is_marked(int32 team)
{
	bool marked = false;
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sLock);
	for (int32 i = 0; i < sTeamCount; i++) {
		if (sTeams[i] == team) {
			marked = true;
			break;
		}
	}
	release_spinlock(&sLock);
	restore_interrupts(state);
	return marked;
}


static bool
team_mark(int32 team)
{
	bool ok = true;
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sLock);
	bool known = false;
	for (int32 i = 0; i < sTeamCount; i++)
		known = known || sTeams[i] == team;
	if (!known) {
		if (sTeamCount < MAX_TEAMS)
			sTeams[sTeamCount++] = team;
		else
			ok = false;
	}
	release_spinlock(&sLock);
	restore_interrupts(state);
	return ok;
}


static void
team_unmark(int32 team)
{
	cpu_status state = disable_interrupts();
	acquire_spinlock(&sLock);
	for (int32 i = 0; i < sTeamCount; i++) {
		if (sTeams[i] == team) {
			sTeams[i] = sTeams[--sTeamCount];
			break;
		}
	}
	release_spinlock(&sLock);
	restore_interrupts(state);
}


// #pragma mark - the patch


// what the jump to user_debug_breakpoint_hit() goes to
static void
hook_breakpoint_hit(bool software)
{
	iframe* frame = x86_get_current_iframe();
	if (frame != NULL && (frame->cs & 3) == 3 && team_is_marked(team_get_current_team_id())) {
		atomic_add(&sHandled, 1);
		// the kernel had put the instruction pointer back on the int3; a Linux kernel leaves it after it
		frame->ip += 1;
		send_signal_etc(find_thread(NULL), SIGTRAP, 0);
		return;
	}
	atomic_add(&sPassed, 1);
	user_debug_breakpoint_hit(software);
}


// finds the jump; returns the number of such jumps in the function (the patch needs exactly one)
static int
find_site(addr_t* siteOut, int* prefixOk)
{
	const unsigned char* start = (const unsigned char*)&x86_handle_breakpoint_exception;
	int found = 0;
	*siteOut = 0;
	*prefixOk = 0;
	for (size_t i = 5; i < SCAN_BYTES; i++) {
		if (start[i] != 0xE9)		// jmp rel32 (the compiler made a tail call of it)
			continue;
		int32 r;
		memcpy(&r, start + i + 1, 4);
		if ((addr_t)((addr_t)start + i + 5 + (int64)r) != (addr_t)&user_debug_breakpoint_hit)
			continue;
		*siteOut = (addr_t)start + i;
		// "mov $1, %edi": the argument (software = true) the original function gives
		static const unsigned char kMovEdi1[5] = { 0xBF, 0x01, 0x00, 0x00, 0x00 };
		*prefixOk = memcmp(start + i - 5, kMovEdi1, 5) == 0 ? 1 : 0;
		found++;
	}
	return found;
}


static status_t
apply_patch()
{
	if (sPatched)
		return B_OK;

	addr_t site;
	int prefixOk;
	int found = find_site(&site, &prefixOk);
	if (found != 1 || !prefixOk) {
		dprintf("wine_int3: found %d jumps to user_debug_breakpoint_hit (prefix %d); not patching\n", found, prefixOk);
		return B_NOT_SUPPORTED;
	}
	const unsigned char* code = (const unsigned char*)site;
	int32 rel;
	memcpy(&rel, code + 1, 4);

	int64 newRel = (int64)((addr_t)&hook_breakpoint_hit - (site + 5));
	if (newRel != (int64)(int32)newRel) {
		dprintf("wine_int3: the driver (%p) is out of reach of the jump (%p); not patching\n",
				(void*)&hook_breakpoint_hit, (void*)site);
		return B_NOT_SUPPORTED;
	}

	// the text is read-only: write through a second mapping of the same physical page
	physical_entry entry;
	status_t status = get_memory_map((const void*)(site + 1), 4, &entry, 1);
	if (status != B_OK) {
		dprintf("wine_int3: get_memory_map failed: %d\n", (int)status);
		return status;
	}
	phys_addr_t page = (phys_addr_t)entry.address & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	size_t offset = (size_t)((phys_addr_t)entry.address - page);
	void* mapped = NULL;
	sPatchArea = map_physical_memory("wine_int3 patch", page, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
									 B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &mapped);
	if (sPatchArea < 0) {
		dprintf("wine_int3: map_physical_memory failed: %d\n", (int)sPatchArea);
		return sPatchArea;
	}
	sPatchAddress = (volatile unsigned char*)mapped + offset;
	sOriginalOperand = rel;

	int32 operand = (int32)newRel;
	memcpy((void*)sPatchAddress, &operand, 4);
	__asm__ volatile("mfence" ::: "memory");

	// look at it from the kernel's own address
	memcpy(&rel, code + 1, 4);
	if (rel != operand) {
		dprintf("wine_int3: the patch did not show in the kernel's text (%d != %d)\n", (int)rel, (int)operand);
		memcpy((void*)sPatchAddress, &sOriginalOperand, 4);
		delete_area(sPatchArea);
		sPatchArea = -1;
		return B_ERROR;
	}
	dprintf("wine_int3: patched the jump at %p to go through %p\n", (void*)site, (void*)&hook_breakpoint_hit);
	sPatched = true;
	return B_OK;
}


static void
remove_patch()
{
	if (!sPatched)
		return;
	memcpy((void*)sPatchAddress, &sOriginalOperand, 4);
	__asm__ volatile("mfence" ::: "memory");
	sPatched = false;
	// (a breakpoint being handled on another CPU at this very moment would still be inside the hook: the module stays loaded
	// while a device is open, and unloading is only done when no one is using it)
	snooze(100000);
	delete_area(sPatchArea);
	sPatchArea = -1;
	dprintf("wine_int3: patch removed\n");
}


// #pragma mark - device


static status_t
device_open(const char*, uint32, void** cookie)
{
	open_cookie* c = (open_cookie*)malloc(sizeof(open_cookie));
	if (c == NULL)
		return B_NO_MEMORY;
	c->team = -1;
	*cookie = c;
	return B_OK;
}


static status_t
device_close(void*)
{
	return B_OK;
}


static status_t
device_free(void* cookie)
{
	open_cookie* c = (open_cookie*)cookie;
	if (c != NULL) {
		if (c->team >= 0)
			team_unmark(c->team);
		free(c);
	}
	return B_OK;
}


static status_t
device_read(void*, off_t, void*, size_t* length)
{
	*length = 0;
	return B_OK;
}


static status_t
device_write(void*, off_t, const void*, size_t* length)
{
	return B_NOT_ALLOWED;
}


static status_t
device_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	open_cookie* c = (open_cookie*)cookie;

	if (op == WINE_INT3_STATUS && length >= sizeof(wine_int3_status)) {
		wine_int3_status status;
		status.version = 1;
		status.patched = sPatched ? 1 : 0;
		status.handled = sHandled;
		status.passed = sPassed;
		status.marked_teams = sTeamCount;
		return user_memcpy(buffer, &status, sizeof(status));
	}
	if (op == WINE_INT3_PROBE && length >= sizeof(wine_int3_probe)) {
		wine_int3_probe probe;
		addr_t site;
		probe.found = find_site(&site, &probe.prefix_ok);
		probe.reachable = 0;
		probe.offset = -1;
		if (probe.found == 1) {
			int64 newRel = (int64)((addr_t)&hook_breakpoint_hit - (site + 5));
			probe.reachable = newRel == (int64)(int32)newRel ? 1 : 0;
			probe.offset = (int32)(site - (addr_t)&x86_handle_breakpoint_exception);
		}
		return user_memcpy(buffer, &probe, sizeof(probe));
	}
	if (op == WINE_INT3_MARK) {
		if (c == NULL)
			return B_BAD_VALUE;
		status_t status = apply_patch();		// only now, when the first team asks
		if (status != B_OK)
			return status;
		int32 team = team_get_current_team_id();
		if (!team_mark(team))
			return B_NO_MEMORY;
		c->team = team;
		return B_OK;
	}
	return B_DEV_INVALID_IOCTL;
}


static device_hooks sDeviceHooks = {
	device_open, device_close, device_free, device_control, device_read, device_write, NULL, NULL, NULL, NULL
};


extern "C" status_t
init_hardware(void)
{
	return B_OK;
}


extern "C" status_t
init_driver(void)
{
	// nothing is patched until a team asks (WINE_INT3_MARK)
	return B_OK;
}


extern "C" void
uninit_driver(void)
{
	remove_patch();
}


extern "C" const char**
publish_devices(void)
{
	static const char* names[] = { DEVICE_NAME, NULL };
	return names;
}


extern "C" device_hooks*
find_device(const char*)
{
	return &sDeviceHooks;
}
