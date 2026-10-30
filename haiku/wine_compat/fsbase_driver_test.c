// Tests the wine_compat FS-base ioctls: finds Thread::user_local_storage from two threads, then sets a worker's FS base
// and checks it survives heavy preemption. (The worker never uses libc after the override.)
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <Drivers.h>

#define LOW 0x40000000UL
#define OP_SET (B_DEVICE_OP_CODES_END + 4)
#define OP_PROBE (B_DEVICE_OP_CODES_END + 5)
struct probe { uint32_t samples, candidates; int32_t offset; };

static int gFd;
static volatile int gRc = -999, gDone;
static volatile uint64_t gNow, gLater;

static void* spin(void* a) { for (;;) {} return a; }
static void* prober(void* a) { struct probe p; ioctl(gFd, OP_PROBE, &p, sizeof p); *(struct probe*)a = p; return NULL; }

static void* worker(void* a) {
	uint64_t magic = 0xC0FFEE5678ULL;
	memcpy((void*)LOW, &magic, 8);
	uint64_t base = LOW;
	gRc = ioctl(gFd, OP_SET, &base, sizeof base);
	if (gRc == 0) {
		__asm__ volatile("movq %%fs:0, %0" : "=r"(gNow));
		volatile uint64_t c = 0; while (c < 300000000ULL) c++;
		__asm__ volatile("movq %%fs:0, %0" : "=r"(gLater));
	}
	gDone = 1;
	for (;;) {}
	return a;
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	gFd = open("/dev/misc/wine_compat", O_RDWR);
	if (gFd < 0) { printf("cannot open the device\n"); return 1; }
	struct probe p[3];
	for (int i = 0; i < 3; i++) {
		pthread_t t; pthread_create(&t, NULL, prober, &p[i]); pthread_join(t, NULL);
		printf("probe %d: samples=%u candidates=%u offset=%d\n", i, p[i].samples, p[i].candidates, p[i].offset);
	}
	if (p[2].offset < 0) { printf("offset not confirmed; stopping\n"); return 2; }
	if (mmap((void*)LOW, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != (void*)LOW) return 3;
	for (int i = 0; i < 32; i++) { pthread_t t; pthread_create(&t, NULL, spin, NULL); }
	pthread_t w; pthread_create(&w, NULL, worker, NULL);
	while (!gDone) usleep(10000);
	printf("set rc=%d, now=0x%lx later=0x%lx -> %s\n", gRc, (unsigned long)gNow, (unsigned long)gLater,
		gLater == 0xC0FFEE5678ULL ? "SURVIVED" : "RESET");
	return gLater == 0xC0FFEE5678ULL ? 0 : 4;
}
