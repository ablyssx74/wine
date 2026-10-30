/*
 * wine_compat: a prototype kernel driver for running 32-bit code inside 64-bit processes (what Wine's WoW64 does).
 *
 * The x86_64 kernel (hrev60264) can run a thread in 32-bit compatibility mode, but when such a thread takes an exception
 * the signal frame is set up by arch_setup_signal_frame(), which only changes the instruction and stack pointers. The
 * code segment stays the 32-bit one, so the 64-bit signal wrapper in the commpage then runs in 32-bit mode and the thread
 * hangs. This driver makes handle_signals() call a function of ours instead, which calls the original and then puts the
 * 64-bit code and stack segments in the interrupt frame when the fault came from 32-bit code. (The context given to the
 * handler keeps the 32-bit CS, and sigreturn restores it, so the thread goes on in 32-bit mode afterwards.)
 *
 * It patches one call instruction in the kernel's text, after checking byte for byte that it is the one it expects, and does
 * nothing at all if the kernel is not the one it was written for. The patch lives in memory only: unloading the driver or
 * rebooting puts the original back.
 *
 * It also exposes two diagnostic-only ioctls (WINE_COMPAT_SET_FS_BASE / GET_FS_BASE) unrelated to the patch above, for
 * experimenting with the thread data segment (%fs) that 32-bit Windows code needs. They are plain WRMSR/RDMSR of
 * IA32_MSR_FS_BASE from kernel context, nothing more.
 */
#include <Drivers.h>
#include <KernelExport.h>
#include <OS.h>
#include <string.h>

#define DEVICE_NAME "misc/wine_compat"

// The call to arch_setup_signal_frame() inside handle_signals() is found at load time (the only E8 rel32 in the body of
// handle_signals() whose target is arch_setup_signal_frame()); nothing here depends on the kernel's build.
extern "C" void handle_signals();
#define USER32_CODE_SELECTOR	0x1b
#define USER_CODE_SELECTOR		0x2b
#define USER_DATA_SELECTOR		0x23

struct iframe {
	uint64 type;
	void* fpu;
	uint64 r15, r14, r13, r12, r11, r10, r9, r8, bp, si, di, dx, cx, bx, ax;
	uint64 orig_rax, vector, error_code;
	uint64 ip, cs, flags, user_sp, user_ss;
};

extern "C" status_t arch_setup_signal_frame(void* thread, void* action, void* frameData);
extern "C" iframe* x86_get_current_iframe(void);

struct wine_compat_status {
	uint32 version;
	uint32 patched;
	int32 calls;			// signals set up
	int32 compat_frames;	// of which from 32-bit code
};
#define WINE_COMPAT_STATUS		(B_DEVICE_OP_CODES_END + 1)

// Diagnostic only, for the FS_BASE experiment: writes/reads the IA32_MSR_FS_BASE MSR directly for the CALLING
// thread, on the CPU it is running on right now. This is a plain WRMSR/RDMSR from kernel context, not tied to the
// signal-frame patch above, and nothing here touches Thread::user_local_storage or survives a context switch by
// itself -- the point of the experiment is to see exactly how far a one-shot WRMSR gets on its own.
#define WINE_COMPAT_SET_FS_BASE	(B_DEVICE_OP_CODES_END + 2)
#define WINE_COMPAT_GET_FS_BASE	(B_DEVICE_OP_CODES_END + 3)
#define IA32_MSR_FS_BASE			0xc0000100

// Persistent FS base: the kernel reloads FS_BASE from Thread::user_local_storage on every context switch, so setting the
// MSR alone is wiped. This ioctl also stores the value in that field. The field's offset in Thread is not known to the
// driver: it is found at run time (the word that equals the thread's current FS_BASE, in at least two threads whose base
// differs) and the ioctl refuses to write anything until exactly one offset has been confirmed.
#define WINE_COMPAT_SET_THREAD_FS_BASE	(B_DEVICE_OP_CODES_END + 4)
#define WINE_COMPAT_PROBE				(B_DEVICE_OP_CODES_END + 5)
#define THREAD_SCAN_WORDS				192			// the first 1536 bytes of the Thread object
struct wine_compat_probe {
	uint32 samples;			// threads looked at
	uint32 candidates;		// offsets still matching in all of them
	int32 offset;			// the confirmed offset, or -1
};

static inline void
write_msr(uint32 msr, uint64 value)
{
	uint32 low = (uint32)value, high = (uint32)(value >> 32);
	__asm__ volatile("wrmsr" :: "c"(msr), "a"(low), "d"(high));
}

static inline uint64
read_msr(uint32 msr)
{
	uint32 low, high;
	__asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
	return ((uint64)high << 32) | low;
}

// 32-bit code reads its data through %fs, and in compatibility mode a null %fs selector makes every such access fault no
// matter what base the MSR holds. Haiku does not save %fs per thread (it is never loaded), so the selector must be the same
// non-null one on every CPU for a thread to keep working after it moves. Loading it in 64-bit mode resets the base, hence
// the base is read first and put back.
#define FS_SELECTOR		0x23
static void
load_fs_selector(void*, int)
{
	const uint64 base = read_msr(IA32_MSR_FS_BASE);
	__asm__ volatile("movw %0, %%fs" :: "r"((uint16)FS_SELECTOR));
	write_msr(IA32_MSR_FS_BASE, base);
}


int32 api_version = B_CUR_DRIVER_API_VERSION;

static bool sPatched = false;
static int32 sCalls = 0, sCompatFrames = 0;		// (changed only with atomic_add())
static area_id sPatchArea = -1;
static volatile unsigned char* sPatchAddress = NULL;		// where the call instruction's operand is, through our own mapping
static int32 sOriginalOperand = 0;
static iframe* (*sGetCurrentIframe)(void) = NULL;

static spinlock sProbeLock = B_SPINLOCK_INITIALIZER;
static uint8 sCandidate[THREAD_SCAN_WORDS];
static uint32 sSamples = 0;
static uint64 sSampleBases[4];
static int32 sTlsOffset = -1;


static inline uint64*
current_thread_words()
{
	uint64 thread;
	__asm__("mov %%gs:0, %0" : "=r"(thread));
	return (uint64*)thread;
}


// looks at the calling thread and narrows down where Thread::user_local_storage is
static void
probe_thread()
{
	const uint64 base = read_msr(IA32_MSR_FS_BASE);
	const uint64* words = current_thread_words();
	if (base < 0x1000 || words == NULL)
		return;

	cpu_status state = disable_interrupts();
	acquire_spinlock(&sProbeLock);
	bool known = false;
	for (uint32 i = 0; i < sSamples && i < 4; i++)
		known = known || sSampleBases[i] == base;
	if (!known && sTlsOffset < 0 && sSamples < 4) {
		for (int i = 0; i < THREAD_SCAN_WORDS; i++) {
			bool match = words[i] == base;
			sCandidate[i] = sSamples == 0 ? match : (sCandidate[i] && match);
		}
		sSampleBases[sSamples++] = base;
		int found = -1, count = 0;
		for (int i = 0; i < THREAD_SCAN_WORDS; i++) {
			if (sCandidate[i]) {
				found = i;
				count++;
			}
		}
		if (sSamples >= 2 && count == 1)
			sTlsOffset = found * 8;
	}
	release_spinlock(&sProbeLock);
	restore_interrupts(state);
}


static status_t
hook_setup_signal_frame(void* thread, void* action, void* frameData)
{
	iframe* frame = sGetCurrentIframe != NULL ? sGetCurrentIframe() : NULL;
	const bool compat = frame != NULL && frame->cs == USER32_CODE_SELECTOR;

	const int32 number = atomic_add(&sCalls, 1);
	if (frame != NULL && (compat || number < 12)) {
		dprintf("wine_compat: signal %d: frame %p cs=0x%lx ss=0x%lx ip=0x%lx sp=0x%lx vector=%lu error=0x%lx\n", (int)number,
				(void*)frame, (unsigned long)frame->cs, (unsigned long)frame->user_ss, (unsigned long)frame->ip,
				(unsigned long)frame->user_sp, (unsigned long)frame->vector, (unsigned long)frame->error_code);
	}

	status_t status = arch_setup_signal_frame(thread, action, frameData);

	if (compat && status == B_OK) {
		atomic_add(&sCompatFrames, 1);
		// the signal wrapper is 64-bit code: it must run in a 64-bit code segment (the context it hands to the handler
		// keeps the 32-bit CS the frame had, and restoring that context returns to 32-bit mode)
		frame->cs = USER_CODE_SELECTOR;
		frame->user_ss = USER_DATA_SELECTOR;
		dprintf("wine_compat:   set up (status %d): the handler wrapper will run at ip=0x%lx sp=0x%lx in cs=0x%lx\n",
				(int)status, (unsigned long)frame->ip, (unsigned long)frame->user_sp, (unsigned long)frame->cs);
	} else if (compat) {
		dprintf("wine_compat:   arch_setup_signal_frame failed: %d\n", (int)status);
	}
	return status;
}


static status_t
apply_patch()
{
#ifdef WINE_COMPAT_NO_HOOK		// (for testing what Wine does when the hook cannot be installed)
	return B_NOT_SUPPORTED;
#endif
	// look for the call in handle_signals(): E8 rel32 whose target is arch_setup_signal_frame(). It has to be there once.
	const unsigned char* start = (const unsigned char*)&handle_signals;
	addr_t site = 0;
	int found = 0;
	for (size_t i = 0; i < 6144; i++) {
		if (start[i] != 0xE8)
			continue;
		int32 r;
		memcpy(&r, start + i + 1, 4);
		if ((addr_t)((addr_t)start + i + 5 + (int64)r) == (addr_t)&arch_setup_signal_frame) {
			site = (addr_t)start + i;
			found++;
		}
	}
	if (found != 1) {
		dprintf("wine_compat: found %d calls to arch_setup_signal_frame in handle_signals; not patching\n", found);
		return B_NOT_SUPPORTED;
	}
	const unsigned char* code = (const unsigned char*)site;
	int32 rel;
	memcpy(&rel, code + 1, 4);
	sGetCurrentIframe = &x86_get_current_iframe;

	// can the new call reach us?
	int64 newRel = (int64)((addr_t)&hook_setup_signal_frame - (site + 5));
	if (newRel != (int64)(int32)newRel) {
		dprintf("wine_compat: the driver (%p) is out of reach of the call (%p); not patching\n",
				(void*)&hook_setup_signal_frame, (void*)site);
		return B_NOT_SUPPORTED;
	}

	// the text is read-only: write through a second mapping of the same physical page
	physical_entry entry;
	status_t status = get_memory_map((const void*)(site + 1), 4, &entry, 1);
	if (status != B_OK) {
		dprintf("wine_compat: get_memory_map failed: %d\n", (int)status);
		return status;
	}
	phys_addr_t page = (phys_addr_t)entry.address & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	size_t offset = (size_t)((phys_addr_t)entry.address - page);
	void* mapped = NULL;
	sPatchArea = map_physical_memory("wine_compat patch", page, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
									 B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &mapped);
	if (sPatchArea < 0) {
		dprintf("wine_compat: map_physical_memory failed: %d\n", (int)sPatchArea);
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
		dprintf("wine_compat: the patch did not show in the kernel's text (%d != %d)\n", (int)rel, (int)operand);
		memcpy((void*)sPatchAddress, &sOriginalOperand, 4);
		delete_area(sPatchArea);
		sPatchArea = -1;
		return B_ERROR;
	}
	dprintf("wine_compat: patched the call at %p to go through %p\n", (void*)site, (void*)&hook_setup_signal_frame);
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
	// (a signal being set up on another CPU at this very moment would still be inside the hook: the module stays loaded
	// while a device is open, and unloading is only done when no one is using it)
	snooze(100000);
	delete_area(sPatchArea);
	sPatchArea = -1;
	dprintf("wine_compat: patch removed\n");
}


// #pragma mark - device


static status_t
device_open(const char*, uint32, void** cookie)
{
	*cookie = NULL;
	return B_OK;
}


static status_t
device_close(void*)
{
	return B_OK;
}


static status_t
device_free(void*)
{
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
device_control(void*, uint32 op, void* buffer, size_t length)
{
	if (op == WINE_COMPAT_STATUS && length >= sizeof(wine_compat_status)) {
		wine_compat_status status;
		status.version = 1;
		status.patched = sPatched ? 1 : 0;
		status.calls = sCalls;
		status.compat_frames = sCompatFrames;
		return user_memcpy(buffer, &status, sizeof(status));
	}
	if (op == WINE_COMPAT_PROBE && length >= sizeof(wine_compat_probe)) {
		probe_thread();
		wine_compat_probe probe;
		probe.samples = sSamples;
		probe.offset = sTlsOffset;
		probe.candidates = 0;
		for (int i = 0; i < THREAD_SCAN_WORDS; i++)
			probe.candidates += sSamples > 0 && sCandidate[i];
		return user_memcpy(buffer, &probe, sizeof(probe));
	}
	if (op == WINE_COMPAT_SET_THREAD_FS_BASE && length >= sizeof(uint64)) {
		uint64 base;
		if (user_memcpy(&base, buffer, sizeof(base)) != B_OK)
			return B_BAD_ADDRESS;
		if (sTlsOffset < 0 || base >= 0x800000000000ULL)
			return sTlsOffset < 0 ? B_NOT_SUPPORTED : B_BAD_VALUE;
		cpu_status state = disable_interrupts();	// no context switch between the field and the MSR
		current_thread_words()[sTlsOffset / 8] = base;
		load_fs_selector(NULL, 0);		// (this CPU, in case it was reset since the driver loaded)
		write_msr(IA32_MSR_FS_BASE, base);
		restore_interrupts(state);
		return B_OK;
	}
	if (op == WINE_COMPAT_SET_FS_BASE && length >= sizeof(uint64)) {
		uint64 base;
		if (user_memcpy(&base, buffer, sizeof(base)) != B_OK)
			return B_BAD_ADDRESS;
		write_msr(IA32_MSR_FS_BASE, base);
		return B_OK;
	}
	if (op == WINE_COMPAT_GET_FS_BASE && length >= sizeof(uint64)) {
		uint64 base = read_msr(IA32_MSR_FS_BASE);
		return user_memcpy(buffer, &base, sizeof(base));
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
	// the signal-frame patch is only for the kernel it was written for; the FS-base ioctl works anywhere it can verify itself
	apply_patch();
	call_all_cpus_sync(load_fs_selector, NULL);
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
