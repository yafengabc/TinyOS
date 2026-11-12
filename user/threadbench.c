/* threadbench - where does a kernel thread's time go?
 *
 *   threadbench
 *
 * Three phases doing exactly 3 x ITERS iterations of the same loop:
 *
 *   3 threads    one ITERS each, so the scheduler really rotates
 *   1 thread     3 x ITERS in a single thread (same work, no contention)
 *   inline       3 x ITERS in the caller
 *
 * Each phase reports wall time plus the scheduler counters for that phase
 * (real switches / self-switches / ticks), so a slowdown can be attributed
 * instead of guessed.  On one core the best possible ratio is 100%: the
 * point is the overhead, not speedup.
 *
 * Build with the kthreads feature (configs/c3-kthreads.conf).
 */

#include "lib.h"

#ifdef TOS_KTHREADS

TOS_HEAP(24 * 1024);
TOS_STACK(4 * 1024);

#define ITERS 300000u
#define NTHR  3

static volatile unsigned done[NTHR];
static volatile unsigned iters_goal;

static void __attribute__((noinline)) worker(void *arg)
{
    unsigned id = (unsigned)(int)arg;
    unsigned n = iters_goal;        /* read once: no volatile in the loop */
    unsigned x = 0;

    for (unsigned i = 0; i < n; i++)
        x += (i ^ id);

    if (id < NTHR)
        done[id]++;
    else if (x == 0xdeadbeefu)      /* keep x live without a console write */
        done[0] = 0;
}

static void diff_stats(const struct schedstat *a, const struct schedstat *b,
                       struct schedstat *d)
{
    d->switches = b->switches - a->switches;
    d->self     = b->self - a->self;
    d->ticks    = b->ticks - a->ticks;
}

static unsigned run_threads(int nthreads, unsigned iters, struct schedstat *d)
{
    struct schedstat a, b;
    int tid[4];
    int code;
    unsigned t0, t1;

    for (int i = 0; i < NTHR; i++)
        done[i] = 0;
    iters_goal = iters;

    schedstat(&a);
    t0 = clock_ms();
    for (int i = 0; i < nthreads; i++) {
        tid[i] = kthread_create(worker, (void *)i, 4096);
        if (tid[i] < 0) {
            printf("threadbench: kthread_create %d failed\n", i);
            return 0;
        }
    }
    for (int i = 0; i < nthreads; i++) {
        if (kthread_join(tid[i], &code) < 0) {
            printf("threadbench: join %d failed\n", i);
            return 0;
        }
    }
    t1 = clock_ms();
    schedstat(&b);
    diff_stats(&a, &b, d);
    return t1 - t0;
}

int main(void)
{
    struct schedstat dthreads, dsingle, dinline;
    unsigned ms_threads, ms_single, ms_inline;

    ms_threads = run_threads(NTHR, ITERS, &dthreads);
    if (done[0] != 1 || done[1] != 1 || done[2] != 1) {
        printf("threadbench: only %u/%u/%u threads ran\n",
               done[0], done[1], done[2]);
        return 1;
    }

    ms_single = run_threads(1, ITERS * NTHR, &dsingle);
    if (done[0] != 1) {
        puts("threadbench: single-thread phase did not complete\n");
        return 1;
    }

    {
        struct schedstat a, b;
        unsigned t0, t1;

        /* Same function, same loop bound, same code as the threads use:
         * this is the apples-to-apples baseline.  Comparing against a
         * separate loop with a compile-time trip count measures the
         * compiler's unrolling, not threading. */
        iters_goal = ITERS;
        schedstat(&a);
        t0 = clock_ms();
        for (unsigned id = 0; id < NTHR; id++)
            worker((void *)id);
        t1 = clock_ms();
        schedstat(&b);
        diff_stats(&a, &b, &dinline);
        ms_inline = t1 - t0;
    }

    printf("threadbench: %u threads x %u: %u ms  sw=%u self=%u tick=%u\n",
           (unsigned)NTHR, ITERS, ms_threads,
           dthreads.switches, dthreads.self, dthreads.ticks);
    printf("threadbench: 1 thread  x %u: %u ms  sw=%u self=%u tick=%u\n",
           ITERS * NTHR, ms_single,
           dsingle.switches, dsingle.self, dsingle.ticks);
    printf("threadbench: inline    x %u: %u ms  sw=%u self=%u tick=%u\n",
           ITERS * NTHR, ms_inline,
           dinline.switches, dinline.self, dinline.ticks);
    printf("threadbench: ratio %u%% (100%% = no overhead)\n",
           ms_inline ? (ms_threads * 100u + ms_inline / 2u) / ms_inline : 0);

    /* A blocking call must park only the calling thread. */
    {
        int t, code;

        iters_goal = 0;                 /* sibling exits immediately */
        t = kthread_create(worker, (void *)9, 4096);
        if (t >= 0) {
            sleep(200);
            kthread_join(t, &code);
            puts("threadbench: sleep() in main, sibling kept running\n");
        }
    }

    return 0;
}

#else

int main(void)
{
    puts("threadbench: kernel threads are off "
         "(build with the kthreads feature)\n");
    return 0;
}

#endif
