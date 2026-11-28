/* threadtest - two threads sharing a counter through a mutex.
 *
 * The interesting part is that `make`-style blocking code stays readable:
 * each worker runs three times and yields, and main joins both.  Compile
 * with threads = 1 in the config (menuconfig: Kernel -> Threads). */

#include "lib.h"

#ifdef TOS_THREADS

TOS_HEAP(16 * 1024);
TOS_STACK(4 * 1024);

static co_mutex_t mx;
static int counter;

static void worker(void *arg)
{
    int me = (int)arg;

    for (int i = 0; i < 3; i++) {
        co_mutex_lock(&mx);
        counter++;
        printf("worker %d: %d\n", me, counter);
        co_mutex_unlock(&mx);
        co_yield();
    }
}

int main(void)
{
    int a, b, code;

    co_mutex_init(&mx);
    a = co_create(worker, (void *)1, 4096);
    b = co_create(worker, (void *)2, 4096);
    if (a < 0 || b < 0) {
        puts("co_create failed (heap/stacks?)\n");
        return 1;
    }

    co_join(a, &code);
    co_join(b, &code);
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
