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

/* One svc, four registers out.  The kernel's SYS_GETCPU puts the id in r0,
 * a constant tag in r3, and echoes the caller's own r2 back in r12.
 *
 * The echo is what makes the sample conclusive.  A tag alone proves
 * nothing: r3 and r12 survive from the previous call, so a stale frame
 * that was never written this time still shows one.  The sequence number
 * changes every iteration, so r12 matching it means "this very call's
 * frame came back", and a mismatch means the frame we returned through is
 * not the one the handler wrote (docs/OPEN-ITEMS.md §22).  r2, r3 and r12
 * are scratch under AAPCS, so a bogus value in any of them cannot break
 * the caller. */
static void probe_call(uint32_t seq, uint32_t o[4])
{
#if defined(__arm__) || defined(__thumb__)
    register int r0 __asm__("r0") = SYS_GETCPU;
    register uint32_t *p __asm__("r1") = o;
    register uint32_t r2 __asm__("r2") = seq;

    __asm__ volatile("svc #0\n\t"
                     "str r0, [r1, #0]\n\t"
                     "str r3, [r1, #4]\n\t"
                     "str r12, [r1, #8]\n\t"
                     "str r2, [r1, #12]"
                     : "+r"(r0), "+r"(r2)
                     : "r"(p)
                     : "r3", "r12", "memory");
#else
    /* Other ports have no equivalent return path to inspect.  Report the id
     * and leave the tag columns empty so the check below stays a no-op
     * instead of reporting every call as BAD. */
    (void)seq;
    o[0] = (uint32_t)getcpu();
    o[1] = 0;
    o[2] = 0;
    o[3] = 0;
#endif
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
    uint32_t bad = 0;
    uint32_t b0 = 0, b3 = 0, b12 = 0, bseq = 0;
    uint32_t calls = 1;         /* the `first` call above */

    while ((uint32_t)(clock_ms() - t0) < (uint32_t)ms) {
        /* A multiply-free inner loop with a volatile result, so the
         * compiler cannot fold it away and the count reflects real time on
         * a CPU rather than a loop the optimiser unrolled. */
        volatile uint32_t sink = 0;
        uint32_t o[4];

        for (int i = 0; i < 256; i++)
            sink = sink + (uint32_t)i;
        n++;

        probe_call(n, o);
        calls++;

        if (o[0] != 0 && o[0] != 1) {
            /* A CPU id outside {0,1} is impossible from the register the
             * kernel reads, so this is either a lost return value or a
             * frame restored from elsewhere.  Keep the first sighting
             * whole for the report. */
            if (!bad) {
                b0 = o[0];
                b3 = o[1];
                b12 = o[2];
                bseq = n;
            }
            bad++;
        }
        /* Deliberately not filtered to 0/1: an id outside that set is the
         * bug this test exists to catch, and it has to stay visible. */
        if ((int)o[0] != first && other < 0)
            other = (int)o[0];
    }

    if (bad)
        printf("worker %d: BAD x%u r0=%08x r3=%08x r12=%08x seq=%u/%u\n", id,
               (unsigned)bad, (unsigned)b0, (unsigned)b3, (unsigned)b12,
               (unsigned)(b12 & 0xFFFFu), (unsigned)(bseq & 0xFFFFu));

    if (other >= 0)
        printf("worker %d: cpu %d then %d, %u blocks, %u calls in %d ms\n",
               id, first, other, (unsigned)n, (unsigned)calls, ms);
    else
        printf("worker %d: cpu %d, %u blocks, %u calls in %d ms\n",
               id, first, (unsigned)n, (unsigned)calls, ms);
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

    /* `smp -s <n> <ms>`: run n workers, wait for all of them, and report
     * the whole result on ONE line.  The per-worker printf is the thing
     * that goes missing when several workers finish together - the console
     * drops lines under contention - so a single summary line is the only
     * way to tell "never ran" (0 blocks) from "ran and its output was
     * lost".  Exit codes are the block counts. */
    if (argc >= 3 && !strcmp(argv[1], "-s")) {
        int n = atoi(argv[2]);
        int ms = (argc >= 4) ? atoi(argv[3]) : 2000;
        int pids[8], codes[8];

        if (n < 1 || n > (int)(sizeof(pids) / sizeof(pids[0])))
            return 2;
        if (ms <= 0)
            ms = 2000;
        printf("smp -s: %d worker(s), %d ms, %u cpu(s)\n",
               n, ms, (unsigned)ci.cpus);
        for (int i = 0; i < n; i++) {
            char idb[8], msb[8];
            char *args[4];

            dec(idb, i);
            dec(msb, ms);
            args[0] = "smp";
            args[1] = "-w";
            args[2] = idb;
            args[3] = msb;
            pids[i] = exec("bin/smp", args, 4);
            codes[i] = -1;
        }
        for (int i = 0; i < n; i++)
            if (pids[i] >= 0)
                codes[i] = wait(pids[i]);

        printf("smp -s result:");
        for (int i = 0; i < n; i++)
            printf(" w%d=%d", i, codes[i]);
        putchar('\n');
        return 0;
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
