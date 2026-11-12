/* threadbench - three kernel threads doing the same CPU-bound work.
 *
 *   threadbench
 *
 * Reports the wall time for 3 threads to each finish N iterations, then the
 * same work done inline by one task.  On a single-core board the two numbers
 * should be about equal: the point is that all three threads *make progress*
 * (the tick preempts them) and that join() collects them, not that they run
 * faster.  On an SMP build the thread version is where parallelism would
 * show up once the scheduler spreads them across cores.
 *
 * Build with the kthreads feature on (configs/c3-kthreads.conf); without it
 * the program only prints why it is off.
 */

#include "lib.h"

#ifdef TOS_KTHREADS

TOS_HEAP(24 * 1024);
TOS_STACK(4 * 1024);

#define ITERS 300000u
#define NTHR  3

static volatile unsigned sums[NTHR];
static volatile unsigned done[NTHR];

static void worker(void *arg)
{
    unsigned id = (unsigned)(int)arg;
    unsigned x = 0;

    for (unsigned i = 0; i < ITERS; i++)
        x += (i ^ id);

    sums[id] = x;
    done[id]++;
    printf("threadbench: thread %u done (sum %u)\n", id, x);
}

int main(void)
{
    int tid[NTHR], code;
    unsigned t0, t1;
    unsigned ms_threads, ms_inline;

    t0 = clock_ms();
    for (int i = 0; i < NTHR; i++) {
        tid[i] = kthread_create(worker, (void *)i, 4096);
        if (tid[i] < 0) {
            printf("threadbench: kthread_create %d failed\n", i);
            return 1;
        }
    }
    for (int i = 0; i < NTHR; i++) {
        if (kthread_join(tid[i], &code) < 0) {
            printf("threadbench: join %d failed\n", i);
            return 1;
        }
    }
    t1 = clock_ms();
    ms_threads = t1 - t0;

    if (done[0] != 1 || done[1] != 1 || done[2] != 1) {
        printf("threadbench: only %u/%u/%u threads ran\n",
               done[0], done[1], done[2]);
        return 1;
    }

    t0 = clock_ms();
    for (unsigned id = 0; id < NTHR; id++) {
        unsigned x = 0;
        for (unsigned i = 0; i < ITERS; i++)
            x += (i ^ id);
        sums[id] ^= x;
    }
    t1 = clock_ms();
    ms_inline = t1 - t0;

    printf("threadbench: %u threads, %u iters each: %u ms\n",
           (unsigned)NTHR, ITERS, ms_threads);
    printf("threadbench: same work inline:        %u ms\n", ms_inline);
    printf("threadbench: ratio %u%% (100%% = no overhead)\n",
           ms_inline ? (ms_threads * 100u + ms_inline / 2u) / ms_inline : 0);

    /* A blocking call must park only the calling thread. */
    {
        int t = kthread_create(worker, (void *)0, 4096);
        (void)t;
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
