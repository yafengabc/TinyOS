#include "lib.h"

/* smp - show that the kernel really runs user code on more than one CPU.
 *
 *   smp                    report the topology, then run one worker per CPU
 *   smp -w <n> <ms>        the worker itself (what `smp` spawns)
 *
 * On a uniprocessor this is still useful: it prints cpus=1 and the workers
 * time-share, all reporting cpu 0.  On the SMP build the point is that the
 * two workers report *different* ids while they are running at the same
 * time - each one counts in a tight loop and the counts add up to roughly
 * the single-worker case when the two really did run in parallel, rather
 * than to half of it when they merely took turns.
 *
 * The id is sampled repeatedly, not once: a task can migrate between CPUs
 * at any tick (the run queue is shared), so a worker that only asks at the
 * start would prove nothing.  `migrated` means it saw more than one.
 */

#define WORK_MS 800

/* The library has no itoa (nothing else needs one), and a two-digit number
 * is not worth adding to lib.c: exec() takes the children's arguments as
 * strings, so the worker number and the duration have to be formatted here. */
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

static int worker(int id, int ms)
{
    /* Milliseconds, not nanoseconds, on purpose: a 64-bit multiply would
     * pull in __aeabi_lmul, which the Cortex-M0+ build does not link, and
     * the loop below does not need better resolution than a tick. */
    uint32_t t0 = clock_ms();
    uint32_t n = 0;
    int first = getcpu();
    int other = -1;

    while ((uint32_t)(clock_ms() - t0) < (uint32_t)ms) {
        /* A multiply-free inner loop with a volatile result, so the
         * compiler cannot fold it away and the count reflects real time on
         * a CPU rather than a loop the optimiser unrolled. */
        volatile uint32_t sink = 0;
        for (int i = 0; i < 256; i++)
            sink = sink + (uint32_t)i;
        n++;

        int c = getcpu();
        if (c != first && other < 0)
            other = c;
    }

    if (other >= 0)
        printf("worker %d: cpu %d then %d, %u blocks in %d ms\n",
               id, first, other, (unsigned)n, ms);
    else
        printf("worker %d: cpu %d, %u blocks in %d ms\n",
               id, first, (unsigned)n, ms);
    return (int)(n & 0x7FFFFFFF);
}

int main(int argc, char *argv[])
{
    struct clockinfo ci;

    if (argc >= 3 && !strcmp(argv[1], "-w")) {
        int id = atoi(argv[2]);
        int ms = (argc >= 4) ? atoi(argv[3]) : WORK_MS;

        if (ms <= 0)
            ms = WORK_MS;
        return worker(id, ms);
    }

    if (clockinfo(&ci) < 0) {
        puts("smp: this kernel has no clock\n");
        return 1;
    }

    printf("smp: %u CPU(s), tick %u Hz, this task on cpu %d\n",
           (unsigned)ci.cpus, (unsigned)ci.tick_hz, getcpu());

    if (ci.cpus <= 1) {
        puts("smp: uniprocessor build - nothing to schedule in parallel\n");
        return 0;
    }

    /* Spawn first, wait afterwards: spawning and waiting inside the same
     * loop would serialise the workers and prove nothing. */
    int pids[8];
    int n = (int)ci.cpus;
    if (n > (int)(sizeof(pids) / sizeof(pids[0])))
        n = (int)(sizeof(pids) / sizeof(pids[0]));

    printf("smp: starting %d worker(s), %d ms each\n", n, WORK_MS);
    for (int i = 0; i < n; i++) {
        char idb[8], msb[8];
        char *args[4];

        dec(idb, i);
        dec(msb, WORK_MS);
        args[0] = "smp";
        args[1] = "-w";
        args[2] = idb;
        args[3] = msb;
        pids[i] = exec("bin/smp", args, 4);
        if (pids[i] < 0) {
            printf("smp: cannot start worker %d\n", i);
            n = i;
            break;
        }
    }

    for (int i = 0; i < n; i++)
        wait(pids[i]);

    puts("smp: done\n");
    return 0;
}
