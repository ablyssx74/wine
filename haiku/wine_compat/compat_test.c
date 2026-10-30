// Test for the wine_compat driver: opens /dev/misc/wine_compat (which loads the driver and patches the kernel), then
//   1. runs 32-bit code in a 64-bit process and returns                                  (worked before the driver)
//   2. makes the 32-bit code take an exception (ud2): SIGILL must reach a 64-bit handler with the 32-bit CS in the context,
//      and the handler returning (sigreturn) must continue the 32-bit code, which then returns to 64-bit mode
// and prints the driver's counters. Without the driver, step 2 hangs the thread (run.sh kills the test after 25 seconds).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <Drivers.h>

#define LOW 0x40000000UL
#define WINE_COMPAT_STATUS (B_DEVICE_OP_CODES_END + 1)
struct wine_compat_status { uint32_t version, patched; int32_t calls, compat_frames; };

static unsigned char* page;
static volatile int fault_count, fault_signal;
static volatile uint64_t fault_rip, fault_cs, fault_ss;
static volatile int skip;

static void handler(int sig, siginfo_t* info, void* ctx) {
    (void)info;
    ucontext_t* uc = ctx;
    fault_signal = sig;
    fault_count++;
    if (fault_count > 4) { write(2, "  [handler entered again and again]\n", 36); _exit(2); }
    mcontext_t* mc = &uc->uc_mcontext;
    fault_rip = mc->rip;
    fault_cs = mc->fpu.fp_fxsave.cs;
    fault_ss = mc->fpu.fp_fxsave.ss;
    mc->rip += skip;
}

static uint32_t base = (uint32_t)LOW, resCell, rspCell, contCell, fp32, fp64, code64;

static void setup_common(void) {
    memset(page, 0x90, 0x10000);
    code64 = base + 0x100; fp32 = base + 0x200; fp64 = base + 0x210;
    resCell = base + 0x300; rspCell = base + 0x308; contCell = base + 0x310;
    unsigned char c64[] = { 0x48, 0x8B, 0x24, 0x25, 0,0,0,0, 0xFF, 0x24, 0x25, 0,0,0,0 };
    memcpy(page + 0x100, c64, sizeof c64);
    *(uint32_t*)(page + 0x104) = rspCell;
    *(uint32_t*)(page + 0x10B) = contCell;
    *(uint32_t*)(page + 0x210) = code64; *(uint16_t*)(page + 0x214) = 0x2b;
    *(uint32_t*)(page + 0x300) = 0;
}

static void run32(const unsigned char* code, size_t len) {
    memcpy(page, code, len);
    *(uint32_t*)(page + 0x200) = base; *(uint16_t*)(page + 0x204) = 0x1b;
    __asm__ volatile(
        "movq %%rsp, (%[rspcell])\n\t"
        "leaq 1f(%%rip), %%rax\n\t"
        "movq %%rax, 0x40000310\n\t"
        "ljmp *(%[fp])\n\t"
        "1:\n\t"
        :
        : [rspcell] "r"((uintptr_t)rspCell), [fp] "r"((uintptr_t)fp32)
        : "rax", "memory");
}

static size_t build(unsigned char* out, const unsigned char* middle, size_t mlen) {
    // ds = es = 0x23, then esp = 0x4000F000: a stack below 4 GB (32-bit code only has the low half of a 64-bit stack pointer)
    unsigned char pro[] = { 0x66, 0xB8, 0x23, 0x00, 0x8E, 0xD8, 0x8E, 0xC0, 0xBC, 0x00, 0xF0, 0x00, 0x40 };
    unsigned char epi[] = { 0xC7, 0x05, 0,0,0,0, 0xEE,0xFF,0xC0,0x00, 0xFF, 0x2D, 0,0,0,0 };	// [resCell] = 0xC0FFEE; jmp far [fp64]
    size_t n = 0;
    memcpy(out + n, pro, sizeof pro); n += sizeof pro;
    memcpy(out + n, middle, mlen); n += mlen;
    memcpy(out + n, epi, sizeof epi);
    *(uint32_t*)(out + n + 2) = resCell;
    *(uint32_t*)(out + n + 12) = fp64;
    return n + sizeof epi;
}

static void show_status(int fd, const char* when) {
    struct wine_compat_status st;
    memset(&st, 0, sizeof st);
    if (ioctl(fd, WINE_COMPAT_STATUS, &st, sizeof st) == 0)
        printf("driver %s: patched=%u signals set up=%d of them from 32-bit code=%d\n", when, st.patched, st.calls, st.compat_frames);
    else
        printf("driver %s: no answer\n", when);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int fd = open("/dev/misc/wine_compat", O_RDWR);
    if (fd < 0) {
        printf("cannot open /dev/misc/wine_compat (the driver is not installed, or refused to load: see the syslog)\n");
        return 1;
    }
    show_status(fd, "after loading");

    page = mmap((void*)LOW, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if ((uintptr_t)page != LOW) { printf("no low memory\n"); return 1; }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, 0);

    unsigned char code[128];
    setup_common();
    size_t n = build(code, (const unsigned char*)"", 0);
    run32(code, n);
    printf("1. run 32-bit code and return: %s\n", *(uint32_t*)(page + 0x300) == 0xC0FFEE ? "OK" : "FAILED");

    printf("2. exception in 32-bit code (ud2)...\n");
    setup_common(); fault_count = 0; skip = 2;
    unsigned char mid2[] = { 0x0F, 0x0B };
    n = build(code, mid2, sizeof mid2);
    run32(code, n);
    printf("   signal %d, %d time(s); the context said cs=0x%lx ss=0x%lx; 32-bit code resumed and returned: %s\n",
           fault_signal, fault_count, (unsigned long)fault_cs, (unsigned long)fault_ss,
           *(uint32_t*)(page + 0x300) == 0xC0FFEE ? "YES" : "NO");

    show_status(fd, "at the end");
    return 0;
}
