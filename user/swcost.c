/* swcost - what a context switch costs, in nanoseconds.
 *
 * Measures three things a "real thread" design has to be compared against:
 *
 *   getpid   a syscall that does not schedule (trap in, trap out)
 *   yield    the full scheduler path: trap, sched_switch, frame restore
 *            (with one runnable task the scheduler switches to itself)
 *   thread   the current cooperative thread switch (only when the config
 *            has threads = 1); two threads yield to each other
 *
 * Run one copy for the self-switch number, two copies with
 * `swcost & swcost` for the cross-task number: both tasks yield, so every
 * iteration really does switch away and back.
 *
 * The numbers are wall clock from clock_ns(), divided by iterations; they
 * include the loop and the console-free printf path, so treat them as an
 * upper bound on the switch itself.
 */

#include "lib.h"

#define N 20000

#ifdef TOS_THREADS
TOS_HEAP(16 * 1024);
TOS_STACK(4 * 1024);

static uint32_t t_iters;

static void spinner(void *arg)
{
    (void)arg;
    for (uint32_t i = 0; i < t_iters; i++)
        thread_yield();
}
#endif

int main(void)
{
    uint64_t t0, t1;
    volatile int pid = 0;

    t0 = clock_ns();
    for (int i = 0; i < N; i++)
        pid = getpid();
    t1 = clock_ns();
    printf("swcost: getpid  %u ns/op (%d)\n", (unsigned)((t1 - t0) / N), pid);

    t0 = clock_ns();
    for (int i = 0; i < N; i++)
        yield();
    t1 = clock_ns();
    printf("swcost: yield   %u ns/op\n", (unsigned)((t1 - t0) / N));

#ifdef TOS_THREADS
    {
        int a, code;
        uint32_t half = N / 2;

        t_iters = half;
        a = thread_create(spinner, 0, 4096);
        if (a < 0) {
            puts("swcost: thread_create failed (heap/stacks?)\n");
            return 1;
        }
        t0 = clock_ns();
        for (uint32_t i = 0; i < half; i++)
            thread_yield();
        thread_join(a, &code);
        t1 = clock_ns();
        printf("swcost: thread  %u ns/op\n",
               (unsigned)((t1 - t0) / (2u * half)));
    }
#else
    puts("swcost: threads off in this build (menuconfig: Kernel -> Threads)\n");
#endif

    puts("swcost: run two copies ('swcost & swcost') for a real cross-task switch\n");
    return 0;
}
