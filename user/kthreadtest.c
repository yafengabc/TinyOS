/* kthreadtest - edge cases the threadbench does not touch.
 *
 *   kthreadtest          create/join, exit codes, join-after-exit, double
 *                        join, slot exhaustion, blocking overlap, stack
 *                        overflow inside a thread, sibling switches
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

/* Set from the sleeping thread so the compiler has to materialise its 768-byte
 * frame: a pointer that escapes cannot be scalar-replaced, which is what
 * happened when only two elements were read.  Only valid while that thread
 * lives; ps is printed during that window. */
static volatile char *slow_frame;

static void slow(void *arg)
{
    /* A frame big enough for ps's stk column to show this thread's own stack
     * (S3b), live across the sleep. */
    volatile char pad[768];

    slow_frame = pad;
    pad[0] = 1;
    pad[sizeof(pad) - 1] = 2;
    sleep(200);
    if (slow_frame[0] == 0x7f || slow_frame[sizeof(pad) - 1] == 0x7f)
        kthread_exit(9);                /* never: keeps the frame live */
    woke = 1;
    kthread_exit(*(int *)arg);
}

#define YTHREADS 4
#define YROUNDS  100
static volatile int yran[YTHREADS];

/* Concurrent sbrk() from threads that can run on two CPUs at once.  The
 * process has one break, and a read-modify-write of it in the kernel must be
 * atomic with respect to the other CPU: otherwise two calls return the same
 * block (the sum of returned offsets repeats a number) or the break moves
 * less than the calls asked for.  The offsets are all multiples of SMP_STEP
 * and their sum is 0+1+...+n-1 when every call got its own range. */
#define SMP_THREADS 4
#define SMP_ITERS   400
#define SMP_STEP    16

static uint32_t smp_base;
static volatile uint32_t smp_sum[SMP_THREADS];
static volatile int smp_err[SMP_THREADS];

static void smp_sbrk(void *arg)
{
    int id = (int)arg;
    uint32_t sum = 0;

    for (unsigned i = 0; i < SMP_ITERS; i++) {
        void *p = sbrk(SMP_STEP);

        if ((uint32_t)p < smp_base) {
            smp_err[id] = 1;
            kthread_exit(1);
        }
        sum += ((uint32_t)p - smp_base) / SMP_STEP;
    }
    smp_sum[id] = sum;
    kthread_exit(0);
}

/* cwd is process state: a thread must see the directory the leader set, and
 * a chdir() in the thread must move the leader too (S1). */
static void cwduser(void *arg)
{
    char buf[TOS_PATH_MAX];

    (void)arg;
    if (getcwd(buf, sizeof(buf)) < 0 || strcmp(buf, "/bin") != 0)
        kthread_exit(1);
    if (chdir("/") != 0)
        kthread_exit(2);
    kthread_exit(0);
}

/* A thread stack lives inside the process heap, so no protection region can
 * fence it off.  The kernel plants a guard word under the stack and checks it
 * at every syscall; yielding after each frame means the first frame that
 * crosses the bottom is caught there, instead of quietly eating the heap. */
static void overflow(void *arg)
{
    volatile char pad[256];
    int depth = (int)arg;

    pad[0] = (char)depth;
    yield();
    overflow((void *)(depth + 1));
    if (pad[0] == 0x7f)                 /* keep the array live */
        kthread_exit(1);
}

/* Two threads of one process taking turns: every yield is a real switch, but
 * the window belongs to the process, so the kernel must not program the
 * protection unit again for each one.  Each thread counts into its own slot:
 * a shared `volatile int` incremented by both is a lost-update race, and the
 * test must fail for the kernel's reason, not its own. */
static void yielder(void *arg)
{
    int id = (int)arg;

    for (int i = 0; i < YROUNDS; i++) {
        yield();
        yran[id]++;
    }
    kthread_exit(0);
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
        /* The thread slot now has its own stack range (S3b): ps must show a
         * thrN row in sleep with a real stk/sz, not 0/0 or the leader's. */
        printf("kthreadtest: ps while the thread sleeps:\n");
        ps();
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

    /* S3b: the stack range now belongs to the thread.  Overflowing it must
     * kill that thread and nobody else; the leader keeps running and join
     * still collects the code (139, the same one a user fault uses). */
    {
        int code = 0;

        tid = kthread_create(overflow, (void *)0, 4096);
        ok(tid > 0, "create an overflowing thread");
        code = -1;
        ok(kthread_join(tid, &code) == 0 && code == 139,
           "thread stack overflow killed only the thread");
        ok(1, "leader survived the thread stack overflow");
    }

    /* S1: a thread sees the leader's cwd and its chdir moves the process. */
    {
        char buf[TOS_PATH_MAX];
        int code = -1;

        ok(chdir("/bin") == 0, "leader chdir(/bin)");
        tid = kthread_create(cwduser, 0, 4096);
        ok(kthread_join(tid, &code) == 0 && code == 0,
           "thread saw the leader's cwd and moved it");
        ok(getcwd(buf, sizeof(buf)) > 0 && strcmp(buf, "/") == 0,
           "thread chdir moved the whole process");
    }

    /* A long-lived process that keeps creating threads must get the stack
     * back when join() has the code.  The window is 96 KiB of heap and a
     * 4 KiB stack a cycle, so a leak runs out long before 200. */
    {
        uint32_t before = (uint32_t)sbrk(0);
        int i, code = 0, failed = 0;

        for (i = 0; i < 200; i++) {
            tid = kthread_create(give42, 0, 4096);
            if (tid < 0) {
                failed = i ? i : -1;
                break;
            }
            code = -1;
            if (kthread_join(tid, &code) != 0 || code != 42) {
                failed = 1000 + i;
                break;
            }
        }
        {
            uint32_t after = (uint32_t)sbrk(0);

            printf("kthreadtest: 200 create/join cycle(s), heap %u -> %u bytes%s\n",
                   before, after, failed ? " (stopped early)" : "");
            ok(failed == 0, "200 create/join cycles fit in the heap");
            ok(after - before <= 8192u, "create/join returns the thread stack");
        }
    }

    /* Two CPUs, one break: sbrk() must hand out disjoint blocks. */
    {
        int tids[SMP_THREADS];
        int i, code, bad = 0;
        uint32_t end;
        uint64_t got = 0;
        const uint64_t want = (uint64_t)SMP_THREADS * SMP_ITERS;

        smp_base = (uint32_t)sbrk(0);
        for (i = 0; i < SMP_THREADS; i++) {
            smp_sum[i] = 0;
            smp_err[i] = 0;
            tids[i] = kthread_create(smp_sbrk, (void *)i, 4096);
            bad |= tids[i] <= 0;
        }
        for (i = 0; i < SMP_THREADS; i++) {
            code = -1;
            if (tids[i] > 0 && kthread_join(tids[i], &code) != 0)
                bad = 1;
            if (code != 0)
                bad = 1;
            got += smp_sum[i];
            bad |= smp_err[i];
        }
        end = (uint32_t)sbrk(0);
        printf("kthreadtest: %u threads x %u sbrk(%u): %u bytes, offset sum %u\n",
               (unsigned)SMP_THREADS, (unsigned)SMP_ITERS, (unsigned)SMP_STEP,
               (unsigned)(end - smp_base), (unsigned)got);
        ok(!bad && end - smp_base == want * SMP_STEP,
           "concurrent sbrk accounts for every byte");
        ok(got == want * (want - 1) / 2,
           "concurrent sbrk returns each block once");
    }

    /* The protection units describe the process, not the thread: a switch
     * between two of its threads must reuse them.  If it did not, the count
     * of programs below would grow with the number of yields. */
    {
        struct schedstat a, b;
        int ids[YTHREADS];
        int i, code, bad = 0;

        for (i = 0; i < YTHREADS; i++) {
            yran[i] = 0;
            ids[i] = kthread_create(yielder, (void *)i, 4096);
            bad |= ids[i] <= 0;
        }
        ok(!bad, "create yielding threads");
        schedstat(&a);
        for (i = 0; i < YTHREADS; i++) {
            code = -1;
            if (ids[i] > 0 && kthread_join(ids[i], &code) != 0)
                bad = 1;
            if (code != 0)
                bad = 1;
        }
        schedstat(&b);
        for (i = 0; i < YTHREADS; i++)
            bad |= yran[i] != YROUNDS;
        ok(!bad, "join yielding threads and all rounds ran");
        ok(b.prot - a.prot <= 2,
           "sibling switches reused the protection regions");
        printf("kthreadtest: %u sibling switch(es), %u region set(s)\n",
               b.switches - a.switches, b.prot - a.prot);
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
