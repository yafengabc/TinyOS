/* threadtest - two threads sharing a counter through a mutex.
 *
 * The interesting part is that `make`-style blocking code stays readable:
 * each worker runs three times and yields, and main joins both.  Compile
 * with threads = 1 in the config (menuconfig: Kernel -> Threads). */

#include "lib.h"

#ifdef TOS_THREADS

TOS_HEAP(16 * 1024);
TOS_STACK(4 * 1024);

static mutex_t mx;
static int counter;

static void worker(void *arg)
{
    int me = (int)arg;

    for (int i = 0; i < 3; i++) {
        mutex_lock(&mx);
        counter++;
        printf("worker %d: %d\n", me, counter);
        mutex_unlock(&mx);
        thread_yield();
    }
}

int main(void)
{
    int a, b, code;

    mutex_init(&mx);
    a = thread_create(worker, (void *)1, 4096);
    b = thread_create(worker, (void *)2, 4096);
    if (a < 0 || b < 0) {
        puts("thread_create failed (heap/stacks?)\n");
        return 1;
    }

    thread_join(a, &code);
    thread_join(b, &code);
    printf("done, counter=%d\n", counter);
    return 0;
}

#else

int main(void)
{
    puts("threads are off (menuconfig: Kernel -> Threads)\n");
    return 0;
}

#endif
