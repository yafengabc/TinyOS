/* prodcons - one thread blocks on a pipe while the other produces.
 *
 *   prodcons [chunks]
 *
 * The kernel makes write() sleep when the pipe is full and read() sleep when
 * it is empty.  Before kernel threads, either one parked the whole process;
 * here the producer keeps filling and the consumer keeps draining, and the
 * only way the transfer finishes is that both wake each other.  The checksum
 * proves no byte was lost or duplicated.
 */

#include "lib.h"

#ifdef TOS_KTHREADS

TOS_HEAP(32 * 1024);
TOS_STACK(4 * 1024);

#define CHUNK  512
#define CHUNKS 64

static int pfd[2];
static volatile unsigned got_bytes;
static volatile unsigned got_sum;

static void consumer(void *arg)
{
    unsigned char buf[CHUNK];
    unsigned total = 0, sum = 0;

    (void)arg;
    for (;;) {
        int n = read(pfd[0], buf, CHUNK);

        if (n <= 0)
            break;                  /* EOF: the producer closed its end */
        for (int i = 0; i < n; i++)
            sum += buf[i];
        total += (unsigned)n;
    }
    got_bytes = total;
    got_sum = sum;
    kthread_exit(0);
}

int main(int argc, char *argv[])
{
    unsigned chunks = CHUNKS;
    unsigned char out[CHUNK];
    unsigned sent = 0, sent_sum = 0;
    unsigned t0, t1;
    int tid, code;

    if (argc > 1 && atoi(argv[1]) > 0)
        chunks = (unsigned)atoi(argv[1]);

    if (pipe(pfd) < 0) {
        puts("prodcons: pipe failed\n");
        return 1;
    }

    got_bytes = 0;
    got_sum = 0;
    tid = kthread_create(consumer, 0, 4096);
    if (tid < 0) {
        puts("prodcons: kthread_create failed\n");
        return 1;
    }

    t0 = clock_ms();
    for (unsigned c = 0; c < chunks; c++) {
        for (int i = 0; i < CHUNK; i++) {
            out[i] = (unsigned char)(i ^ (int)c);
            sent_sum += out[i];
        }
        int n = write(pfd[1], out, CHUNK);
        if (n != CHUNK) {
            printf("prodcons: short write %d at chunk %u\n", n, c);
            return 1;
        }
        sent += CHUNK;
    }
    close(pfd[1]);                  /* the consumer reads EOF after draining */

    if (kthread_join(tid, &code) < 0) {
        puts("prodcons: join failed\n");
        return 1;
    }
    t1 = clock_ms();

    printf("prodcons: %u bytes through a %u-byte pipe in %u ms\n",
           sent, (unsigned)CHUNK, t1 - t0);
    printf("prodcons: sent sum %u, received sum %u, %u bytes\n",
           sent_sum, got_sum, got_bytes);

    if (got_bytes != sent || got_sum != sent_sum) {
        puts("prodcons: MISMATCH\n");
        return 1;
    }
    puts("prodcons: all bytes delivered, both threads blocked and woke\n");
    return 0;
}

#else

int main(void)
{
    puts("prodcons: kernel threads are off\n");
    return 0;
}

#endif
