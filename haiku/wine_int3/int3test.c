/* Tests the wine_int3 driver from a native Haiku program:
 *   int3test probe        what the driver found in the kernel (nothing is patched)
 *   int3test status       counters
 *   int3test run [n]      marks this team, then executes int3 n times (default 5) in the main thread and in a second thread,
 *                         with a SIGTRAP handler that counts, and checks that the vector registers survive each one */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <Drivers.h>

#define OP_STATUS (B_DEVICE_OP_CODES_END + 1)
#define OP_PROBE (B_DEVICE_OP_CODES_END + 2)
#define OP_MARK (B_DEVICE_OP_CODES_END + 3)

struct status { uint32_t version, patched; int32_t handled, passed, marked; };
struct probe { int32_t found, prefix_ok, reachable, offset; };

static volatile int traps;
static volatile void *last_ip;

static void on_trap(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si; (void)uc;
    traps++;
}

static int bad_regs;

static void do_traps(int n)
{
    static const unsigned char K[16] __attribute__((aligned(16))) = { 1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31 };
    for (int i = 0; i < n; i++)
    {
        unsigned char back[16] __attribute__((aligned(16)));
        /* load a constant into xmm8-xmm15, trap, and read them back: a lost vector register would show as zero */
        __asm__ volatile(
            "movdqa (%1),%%xmm8\n\tmovdqa (%1),%%xmm9\n\tmovdqa (%1),%%xmm10\n\tmovdqa (%1),%%xmm11\n\t"
            "movdqa (%1),%%xmm12\n\tmovdqa (%1),%%xmm13\n\tmovdqa (%1),%%xmm14\n\tmovdqa (%1),%%xmm15\n\t"
            "int3\n\t"
            "movdqa %%xmm8,(%0)\n\t"
            : : "r"(back), "r"(K) : "memory", "xmm8","xmm9","xmm10","xmm11","xmm12","xmm13","xmm14","xmm15");
        if (memcmp(back, K, 16)) bad_regs++;
    }
}

static void *thread_main(void *arg) { do_traps(*(int *)arg); return NULL; }

int main(int argc, char **argv)
{
    int fd = open("/dev/misc/wine_int3", O_RDWR);
    if (fd < 0) { perror("open /dev/misc/wine_int3"); return 1; }
    if (argc > 1 && !strcmp(argv[1], "probe"))
    {
        struct probe p; memset(&p, 0, sizeof p);
        int rc = ioctl(fd, OP_PROBE, &p, sizeof p);
        printf("probe rc=%d: jumps found %d, 'mov $1,%%edi' before it %d, reachable %d, at offset %d in x86_handle_breakpoint_exception\n", rc, p.found, p.prefix_ok, p.reachable, p.offset);
        return rc != 0;
    }
    if (argc > 1 && !strcmp(argv[1], "status"))
    {
        struct status s; memset(&s, 0, sizeof s);
        int rc = ioctl(fd, OP_STATUS, &s, sizeof s);
        printf("status rc=%d: patched %u, handled %d, passed %d, marked teams %d\n", rc, s.patched, s.handled, s.passed, s.marked);
        return rc != 0;
    }
    if (argc > 1 && !strcmp(argv[1], "run"))
    {
        int n = argc > 2 ? atoi(argv[2]) : 5;
        struct sigaction sa; memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = on_trap; sa.sa_flags = SA_SIGINFO;
        sigaction(SIGTRAP, &sa, NULL);
        int rc = ioctl(fd, OP_MARK, NULL, 0);
        printf("mark rc=%d\n", rc);
        if (rc != 0) return 2;
        fflush(stdout);
        do_traps(n);
        printf("main thread: %d int3 executed, %d SIGTRAP handled, %d register mismatches\n", n, traps, bad_regs);
        fflush(stdout);
        pthread_t t; int n2 = n;
        pthread_create(&t, NULL, thread_main, &n2); pthread_join(t, NULL);
        printf("after the second thread: %d SIGTRAP handled in all, %d register mismatches\n", traps, bad_regs);
        struct status s; memset(&s, 0, sizeof s); ioctl(fd, OP_STATUS, &s, sizeof s);
        printf("driver: patched %u, handled %d, passed %d, marked teams %d\n", s.patched, s.handled, s.passed, s.marked);
        printf(traps == 2 * n && !bad_regs ? "RESULT OK\n" : "RESULT FAILED\n");
        return traps == 2 * n && !bad_regs ? 0 : 3;
    }
    printf("usage: int3test probe|status|run [n]\n");
    return 1;
}
