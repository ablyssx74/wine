// Does 32-bit code (compatibility mode) read its data through %fs with the base the wine_compat ioctl set?
// usage: fs32_test [load]   ("load" first loads %fs with 0x23, a flat 32-bit data segment, and only then sets the base)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <Drivers.h>

#define LOW 0x40000000UL
#define OP_SET (B_DEVICE_OP_CODES_END + 4)
extern int _kern_ioctl(int fd, uint32_t op, void* buffer, size_t length);

static int gFd, gLoad;
static unsigned char* page;
static volatile uint32_t gSeen, gDone, gSig;
static volatile uint64_t gNative;
static volatile uint16_t gSel;

static void segv(int s, siginfo_t* si, void* u) {
	// (runs with whatever %fs the faulting code had: write plainly, then give the native base back and leave)
	gSig = s;
	uint64_t n = gNative;
	_kern_ioctl(gFd, OP_SET, &n, sizeof n);
	gDone = 1;
	for (;;) {}
}

static void* worker(void* a) {
	uint64_t native;
	__asm__ volatile("movq %%fs:0, %0" : "=r"(native));
	gNative = native;
	{ uint16_t sel; __asm__ volatile("movw %%fs, %0" : "=r"(sel)); gSel = sel; printf("worker starts with fs=0x%x\n", sel); }
	uint32_t base = LOW, resCell = LOW + 0x300, rspCell = LOW + 0x308, contCell = LOW + 0x310;
	memset(page, 0x90, 0x10000);
	unsigned char c64[] = { 0x48, 0x8B, 0x24, 0x25, 0,0,0,0, 0xFF, 0x24, 0x25, 0,0,0,0 };
	memcpy(page + 0x100, c64, sizeof c64);
	*(uint32_t*)(page + 0x104) = rspCell;
	*(uint32_t*)(page + 0x10B) = contCell;
	*(uint32_t*)(page + 0x210) = base + 0x100; *(uint16_t*)(page + 0x214) = 0x2b;
	*(uint32_t*)(page + 0x200) = base; *(uint16_t*)(page + 0x204) = 0x1b;
	*(uint32_t*)(page + 0x400) = 0xA5A5F00D;		// what %fs:0 should give
	// 32-bit: ds=es=0x23, esp; mov eax,fs:[0]; mov [resCell],eax; jmp far [fp64]
	unsigned char code[] = { 0x66,0xB8,0x23,0x00, 0x8E,0xD8, 0x8E,0xC0, 0xBC,0x00,0xF0,0x00,0x40,
		0x64,0xA1,0,0,0,0,  0xA3,0,0,0,0,  0xFF,0x2D,0,0,0,0 };
	*(uint32_t*)(code + 20) = resCell;
	*(uint32_t*)(code + 26) = base + 0x210;
	memcpy(page, code, sizeof code);
	*(uint32_t*)(page + 0x300) = 0;

	if (gLoad) { __asm__ volatile("movw $0x23, %%ax\n\tmovw %%ax, %%fs" ::: "rax"); }
	uint64_t fsb = LOW + 0x400;
	_kern_ioctl(gFd, OP_SET, &fsb, sizeof fsb);
	__asm__ volatile(
		"movq %%rsp, (%[rspcell])\n\t"
		"leaq 1f(%%rip), %%rax\n\t"
		"movq %%rax, 0x40000310\n\t"
		"ljmp *(%[fp])\n\t"
		"1:\n\t"
		:: [rspcell] "r"((uintptr_t)rspCell), [fp] "r"((uintptr_t)(LOW + 0x200)) : "rax", "memory");
	gSeen = *(volatile uint32_t*)(page + 0x300);
	_kern_ioctl(gFd, OP_SET, &gNative, sizeof gNative);
	gDone = 1;
	for (;;) {}
	return a;
}

int main(int argc, char** argv) {
	setvbuf(stdout, NULL, _IONBF, 0);
	gLoad = argc > 1;
	gFd = open("/dev/misc/wine_compat", O_RDWR);
	if (gFd < 0) { printf("no device\n"); return 1; }
	page = mmap((void*)LOW, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	struct sigaction sa; memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = segv; sa.sa_flags = SA_SIGINFO;
	sigaction(SIGSEGV, &sa, 0);
	pthread_t t; pthread_create(&t, NULL, worker, NULL);
	while (!gDone) usleep(10000);
	printf("fs selector in the worker: 0x%x\n", gSel);
	if (gSig) printf("fault: signal %u in the 32-bit code\n", gSig);
	else printf("32-bit fs:[0] = 0x%x -> %s\n", gSeen, gSeen == 0xA5A5F00D ? "OK, FS base works in 32-bit mode" : "WRONG");
	return 0;
}
