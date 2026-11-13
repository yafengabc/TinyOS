/* kthreadtest - edge cases the threadbench does not touch.
 *
 *   kthreadtest          create/join, exit codes, join-after-exit, double
 *                        join, slot exhaustion, blocking overlap
 *   kthreadtest leave    exit the leader while a thread is still running;
 *                        the kernel must retire the group (run `ps` after)
 *
 * Every check prints one line; the exit code is the number of failures.
 */

#include "lib.h"

#ifdef TOS_KTHREADS

/* 18 threads x (4112 B stack + 8 B start record) = 74,160 B, plus ~8 KiB
 * of startup malloc.  At a 16 KiB window grain 72 KiB leaves only ~3.6 KiB
 * for the last thread, so kthread_create() silently returns -1 on real
 * hardware.  QEMU's 64 KiB grain rounds the window up and masks it. */
TOS_HEAP(96 * 1024);
TOS_STACK(4 * 1024);

static int fails;
static int codes[16];

static void ok(int cond, const char *what)
{
    if (cond) {
        printf("kthreadtest: ok   %s\n", what);
    } else {
        printf("kthreadtest: FAIL %s\n", what);
        fails++;
    }
}

static void give42(void *arg)
{
    (void)arg;
    kthread_exit(42);
}

static void give(void *arg)
{
    kthread_exit(*(int *)arg);
}

static volatile int woke;

static void slow(void *arg)
{
    sleep(200);
    woke = 1;
    kthread_exit(*(int *)arg);
}

static volatile int fd_read_result = -2;

static void fduser(void *arg)
{
    int fd = *(int *)arg;
    char b;

    fd_read_result = read(fd, &b, 1);   /* 1 while the file is open */
    /* On real hardware the leader's 20 ms sleep can stretch past 70 ms
     * (preemptive SMP + 1 ms tick), so 60 ms here let the thread finish
     * its second read BEFORE the leader's close().  Give the leader a wide
     * margin: the test is about a shared fd table, not a race. */
    sleep(200);                         /* the leader closes it now */
    fd_read_result = read(fd, &b, 1);   /* must be -1: shared table */
    kthread_exit(0);
}

static void forever(void *arg)
{
    (void)arg;
    for (;;)
        sleep(10);
}

int main(int argc, char *argv[])
{
    int code, tid, n, i;
    int tids[16];

    if (argc > 1 && !strcmp(argv[1], "leave")) {
        tid = kthread_create(forever, 0, 4096);
        printf("kthreadtest: leader exits, thread %d still alive\n", tid);
        return 0;
    }

    code = -1;
    tid = kthread_create(give42, 0, 4096);
    ok(tid > 0, "create");
    ok(kthread_join(tid, &code) == 0 && code == 42,
       "join returns the exit code");

    ok(kthread_join(tid, &code) == -1, "second join fails");

    code = -1;
    tid = kthread_create(give42, 0, 4096);
    sleep(100);                 /* let it exit: the slot becomes a zombie */
    ok(kthread_join(tid, &code) == 0 && code == 42,
       "join after exit (zombie slot)");

    ok(kthread_join(9999, &code) == -1, "join unknown tid fails");

    /* Exhaustion: threads exit immediately but hold their slot until joined,
     * exactly like an unwaited child.  What runs out is table slots, not
     * memory: the stacks were carved out of the heap already. */
    n = 0;
    for (i = 0; i < 16; i++) {
        codes[i] = i + 1;
        tid = kthread_create(give, &codes[i], 4096);
        if (tid < 0)
            break;
        tids[n++] = tid;
    }
    printf("kthreadtest: %d thread slot(s) usable beside shell+self\n", n);
    ok(n > 0 && n < 16, "slot exhaustion returns -1");
    for (i = 0; i < n; i++) {
        code = -1;
        if (kthread_join(tids[i], &code) != 0 || code != i + 1) {
            fails++;
            printf("kthreadtest: FAIL churn join %d (code %d)\n", i, code);
        }
    }

    tid = kthread_create(give42, 0, 4096);
    ok(tid > 0, "create works again after joining everything");
    code = -1;
    ok(kthread_join(tid, &code) == 0 && code == 42, "churn join code");

    {
        uint32_t t0, t1;
        int v = 7;

        /* Deterministic overlap test: the thread sleeps 200 ms and only then
         * sets `woke`; the leader sleeps 50 ms and looks.  If sleep() parked
         * the whole process (as the cooperative library does), the leader
         * could not observe woke == 0. */
        woke = 0;
        t0 = clock_ms();
        tid = kthread_create(slow, &v, 4096);
        sleep(50);
        ok(woke == 0, "leader ran while the thread was still asleep");
        code = -1;
        ok(kthread_join(tid, &code) == 0 && code == 7, "join sleeping thread");
        ok(woke == 1, "thread finished before join returned");
        t1 = clock_ms();
        printf("kthreadtest: overlapped sleep %u ms (wall clock)\n", t1 - t0);
    }

    /* Descriptors are group state: a file opened by the leader is readable
     * in a thread, and close() in the leader closes it there too. */
    {
        int fd = open("/readme.txt");
        int junk;

        ok(fd >= 0, "open readme.txt in the leader");
        fd_read_result = -2;
        tid = kthread_create(fduser, &fd, 4096);
        sleep(20);
        ok(fd_read_result == 1, "thread read through the shared fd table");
        close(fd);
        code = -1;
        ok(kthread_join(tid, &code) == 0, "join the fd thread");
        ok(fd_read_result == -1, "leader close() closed it for the thread");
        (void)junk;
    }

    printf("kthreadtest: %s (%d failure(s))\n",
           fails ? "FAILED" : "all ok", fails);
    return fails ? 1 : 0;
}

#else

int main(void)
{
    puts("kthreadtest: kernel threads are off\n");
    return 0;
}

#endif
