#include "lib.h"

/* smpbench - measure the throughput gain from the second CPU.
 *
 *   smpbench [iters]                one worker, then two, each doing iters
 *   smpbench -w <iters> <fd> <seed> the worker itself
 *
 * The worker runs a fixed amount of arithmetic and returns its elapsed
 * milliseconds as the exit code.  One worker alone gives T1; two workers,
 * each doing the same work in parallel, should take T2 ~= T1 if the second
 * CPU really helps, so the speedup is 2*T1/max(T2a,T2b).  Fixed work rather
 * than "run for N ms" keeps the measurement independent of the clock's
 * resolution and of how the two workers start relative to each other.
 *
 * The start is synchronised through one pipe per worker: each worker blocks
 * on a byte, and the parent writes both only after both execs returned.
 * The hot loop has no syscall, no lock and no console output.
 */

#define BLK 4096
#define DEFAULT_ITERS 5000000u
#define ROUNDS 2

static void dec(char *buf, int v)
{
    char tmp[12];
    int i = 0;

    do {
        tmp[i++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0 && i < (int)sizeof(tmp));
    for (int k = 0; k < i; k++)
        buf[k] = tmp[i - 1 - k];
    buf[i] = '\0';
}

static int worker(unsigned iters, int fd, unsigned seed)
{
    volatile uint32_t sink = 0;
    uint32_t x = seed | 1u;
    uint32_t t0, t1;
    char go;

    if (read(fd, &go, 1) != 1)
        return -1;
    t0 = clock_ms();
    for (uint32_t done = 0; done < iters; done += BLK) {
        for (int i = 0; i < BLK; i++) {
            x = x * 1664525u + 1013904223u;
            x ^= x >> 13;
        }
        sink += x;
    }
    t1 = clock_ms();
    if (sink == 0xFFFFFFFFu)
        return 0;
    return (int)(t1 - t0);
}

/* Run `count` workers with the same iteration count; return their elapsed
 * milliseconds in out[]. */
static int run(int count, unsigned iters, int out[2])
{
    int fds[2][2];
    int pid[2];
    char kb[16], fb[8], sb[8];
    char go = 1;
    int made = 0;

    for (int i = 0; i < count; i++) {
        char *args[5];
        int r, w;

        if (pipe(fds[i]) < 0)
            break;
        r = fds[i][0];
        w = fds[i][1];
        dec(kb, (int)iters);
        dec(fb, r);
        dec(sb, 1000 + i * 7919);
        args[0] = "smpbench";
        args[1] = "-w";
        args[2] = kb;
        args[3] = fb;
        args[4] = sb;
        pid[i] = exec("bin/smpbench", args, 5);
        close(r);
        if (pid[i] < 0) {
            close(w);
            break;
        }
        fds[i][1] = w;
        made++;
    }

    for (int i = 0; i < made; i++) {
        write(fds[i][1], &go, 1);
        close(fds[i][1]);
    }
    for (int i = 0; i < made; i++)
        out[i] = wait(pid[i]);
    return made;
}

int main(int argc, char *argv[])
{
    unsigned iters = DEFAULT_ITERS;
    int is_child = argc >= 2 && argv[1][0] == '-';

    if (is_child) {
        if (argc < 5)
            return -1;
        int myfd = atoi(argv[3]);
        unsigned seed = (unsigned)atoi(argv[4]);

        for (int fd = 3; fd < 12; fd++)
            if (fd != myfd)
                close(fd);
        return worker((unsigned)atoi(argv[2]), myfd, seed);
    }

    if (argc >= 2) {
        int v = atoi(argv[1]);
        if (v >= (int)BLK)
            iters = (unsigned)v;
    }

    unsigned t1 = 0, t2 = 0;
    int a = 0, b = 0;
    int good = 0;

    printf("smpbench: %u iterations per worker, best of %d\n",
           iters, ROUNDS);
    for (int r = 0; r < ROUNDS; r++) {
        int one[2] = { 0, 0 }, two[2] = { 0, 0 };

        if (run(1, iters, one) != 1 || one[0] <= 0)
            continue;
        if (run(2, iters, two) != 2 || two[0] <= 0 || two[1] <= 0)
            continue;
        if (!t1 || (unsigned)one[0] < t1)
            t1 = (unsigned)one[0];
        {
            unsigned w = (unsigned)(two[0] > two[1] ? two[0] : two[1]);

            if (!t2 || w < t2) {
                t2 = w;
                a = two[0];
                b = two[1];
            }
        }
        good++;
    }
    if (!good || !t1 || !t2) {
        printf("  worker failed\n");
        return 1;
    }

    printf("  single: %u ms\n", t1);
    printf("  dual:   %d + %d ms (wall %u)\n", a, b, t2);
    /* 2*T1/T2 in hundredths; counts are milliseconds, so no overflow. */
    unsigned ratio = t2 ? (2u * t1 * 100u) / t2 : 0;
    printf("  speedup: %u.%02ux\n", ratio / 100u, ratio % 100u);
    return 0;
}
