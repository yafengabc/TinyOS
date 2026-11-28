/* kthreadtest - edge cases the threadbench does not touch.
 *
 *   kthreadtest          create/join, exit codes, join-after-exit, double
 *                        join, slot exhaustion, blocking overlap, stack
 *                        overflow inside a thread, sibling switches
 *   kthreadtest leave    exit the leader while a thread is still running;
 *                        the kernel must retire the group (run `ps` after)
 *   kthreadtest pipes        exit the leader while it holds a pipe whose
 *                            reader is blocked (deadlock probe)
 *   kthreadtest create-exit  exit the leader while a sibling creates (slot
 *                            recycling probe)
 *   kthreadtest double-join  two siblings join one tid (parking probe)
 *
 * Every check prints one line; the exit code is the number of failures.
 */

#include "lib.h"

#ifdef TOS_KTHREADS

/* The heap is malloc and the 512-byte fallback stacks of the exhaustion
 * test, not the 4 KiB thread stacks: on a stack-area board (the C3) those
 * come from the area at the top of the user region and cost no heap.  On a
 * board without one, 18 x (4112 B + 8 B record) is ~74 KiB and the 96 KiB
 * keeps the last thread from failing for want of a window. */
TOS_HEAP(96 * 1024);
TOS_STACK(4 * 1024);

static int fails;
static int codes[40];

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
    thread_exit(42);
}

static void give(void *arg)
{
    thread_exit(*(int *)arg);
}

static volatile int woke;

/* Blocking mutex: one thread holds the word, another must sleep in the
 * kernel (ps state "mutex") rather than spin, and take over on unlock.  The
 * second case is the heap-lock nightmare: a holder that faults must hand the
 * word on, or every sibling waits forever. */
static int tmutex, deadmutex;
static volatile int locker_got;

static void locker(void *arg)
{
    (void)arg;
    thread_mutex_lock(&tmutex);
    locker_got = 1;
    sleep(20);
    thread_mutex_unlock(&tmutex);
    thread_exit(0);
}

/* The last word of the 32-bit space: unmapped on every port this OS runs on.
 *
 * Address 0 is NOT a usable probe everywhere.  On RP2350's Hazard3 the bootrom
 * leaves hardwired RWX PMP entries over the ROM region (pmp_map.h: "8..10 are
 * hardwired by the bootrom ... writing those is a no-op"), so a store to 0 is
 * legal there - measured: `prot dev 0x0` and `prot null` both survive in U
 * mode while `prot heap`, `prot stack` and an unmapped address all fault.  On
 * top of that this probe runs as a kthread, and arch/rv32/platform.c returns
 * ktasks to M mode on mret, where PMP does not apply at all.
 *
 * A word in the top page is outside every map, so the access faults for the
 * reason this case is about - a thread dies while holding a mutex - instead of
 * depending on what a particular bootrom happened to leave in PMP. */
#define FAULT_PROBE_ADDR 0xFFFFFFF0u

static void die_holding(void *arg)
{
    (void)arg;
    thread_mutex_lock(&deadmutex);
    *(volatile int *)FAULT_PROBE_ADDR = 1;   /* fault holding the word */
}

/* Concurrent malloc/free from four threads.  The free list is shared, and
 * even on a uniprocessor the tick can preempt a thread in the middle of it;
 * the checksum catches a block handed out twice (two threads writing each
 * other's pattern).  This failed before user/heap.c took the kernel mutex. */
#define HEAP_THREADS 4
#define HEAP_ITERS   200
static volatile int heap_bad[HEAP_THREADS];

static void heaper(void *arg)
{
    int id = (int)arg;

    for (int i = 0; i < HEAP_ITERS; i++) {
        unsigned n = 32u + (unsigned)((i * 37 + id * 11) % 480);
        volatile char *a = malloc(n);
        volatile char *b = malloc(n);

        if (!a || !b) {
            heap_bad[id] = 1;
            thread_exit(1);
        }
        for (unsigned j = 0; j < n; j++) {
            a[j] = (char)(j ^ (unsigned)id);
            b[j] = (char)(j ^ (unsigned)(id + 1));
        }
        for (unsigned j = 0; j < n; j++) {
            if (a[j] != (char)(j ^ (unsigned)id) ||
                b[j] != (char)(j ^ (unsigned)(id + 1))) {
                heap_bad[id] = 2;
                break;
            }
        }
        free((void *)a);
        free((void *)b);
        if (heap_bad[id])
            thread_exit(2);
    }
    thread_exit(0);
}

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
        thread_exit(9);                /* never: keeps the frame live */
    woke = 1;
    thread_exit((int)arg);
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
static volatile int smp_go;

static void smp_sbrk(void *arg)
{
    int id = (int)arg;
    uint32_t sum = 0;

    /* The baseline is taken by the leader after this thread's stack has been
     * allocated.  On a board without a stack area thread_create() gets the
     * stack from malloc, and malloc asks sbrk() for a new arena when the
     * free list has nothing big enough; letting that happen inside the
     * measured window counted the allocator's growth as if the threads had
     * asked for those bytes. */
    while (!smp_go)
        ;

    for (unsigned i = 0; i < SMP_ITERS; i++) {
        void *p = sbrk(SMP_STEP);

        if ((uint32_t)p < smp_base) {
            smp_err[id] = 1;
            thread_exit(1);
        }
        sum += ((uint32_t)p - smp_base) / SMP_STEP;
    }
    smp_sum[id] = sum;
    thread_exit(0);
}

/* cwd is process state: a thread must see the directory the leader set, and
 * a chdir() in the thread must move the leader too (S1). */
static void cwduser(void *arg)
{
    char buf[TOS_PATH_MAX];

    (void)arg;
    if (getcwd(buf, sizeof(buf)) < 0 || strcmp(buf, "/bin") != 0)
        thread_exit(1);
    if (chdir("/") != 0)
        thread_exit(2);
    thread_exit(0);
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
        thread_exit(1);
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
    thread_exit(0);
}

static volatile int fd_read_result = -2;

static void fduser(void *arg)
{
    int fd = (int)arg;
    char b;

    fd_read_result = read(fd, &b, 1);   /* 1 while the file is open */
    /* On real hardware the leader's 20 ms sleep can stretch past 70 ms
     * (preemptive SMP + 1 ms tick), so 60 ms here let the thread finish
     * its second read BEFORE the leader's close().  Give the leader a wide
     * margin: the test is about a shared fd table, not a race. */
    sleep(200);                         /* the leader closes it now */
    fd_read_result = read(fd, &b, 1);   /* must be -1: shared table */
    thread_exit(0);
}

static void forever(void *arg)
{
    (void)arg;
    for (;;)
        sleep(10);
}

/* --- probes for the thread-exit defects found in the E1/E2 review --------
 *
 * Each one is named explicitly instead of joining the default run, because
 * until the kernel side is fixed the expected outcome is a deadlock or a
 * leaked slot, which would take every later check down with it:
 *
 *   kthreadtest pipes        the leader exits while it still holds a pipe
 *                            whose only reader is blocked.  Closing the
 *                            descriptors needs proc_lock, and the exit path
 *                            already holds it -> self deadlock, no prompt.
 *                            A healthy kernel returns status 3.
 *   kthreadtest create-exit  a sibling spins in create/join while the leader
 *                            exits.  A thread created into a group that is
 *                            already dying must not survive, and must never
 *                            wake up owning some other process's slot.
 *   kthreadtest double-join  two siblings join the same tid at once.  Only
 *                            one may collect the code; the other has to be
 *                            refused rather than parked forever.
 */

static void pipe_blocked(void *arg)
{
    int fd = (int)arg;
    char b;

    (void)read(fd, &b, 1);          /* sleeps on the empty pipe */
    thread_exit(0);
}

static int probe_pipes(void)
{
    int fds[2];
    int tid;

    if (pipe(fds) != 0) {
        printf("kthreadtest: FAIL pipe()\n");
        return 1;
    }
    tid = thread_create(pipe_blocked, (void *)fds[0], 4096);
    if (tid <= 0) {
        printf("kthreadtest: FAIL create the blocked reader\n");
        return 1;
    }
    sleep(20);                      /* let it reach read() and block */
    printf("kthreadtest pipes: leader exits holding %d/%d, reader %d asleep\n",
           fds[0], fds[1], tid);
    printf("kthreadtest pipes: expect status 3 and a prompt (no prompt = "
           "deadlock)\n");
    return 3;
}

static volatile int churn_go;
static volatile int churn_bad;
static volatile int churn_n;

static void churn_born(void *arg)
{
    /* The window this closes: created into a group that is exiting, the
     * new thread can be handed a proc slot that has already been recycled,
     * and then reads some other program's pid. */
    if (getpid() != (int)arg)
        churn_bad = 3;
    thread_exit(0);
}

static void churn_worker(void *arg)
{
    int me = getpid();
    int tid;

    (void)arg;
    while (churn_go) {
        tid = thread_create(churn_born, (void *)me, 512);
        if (tid <= 0)
            break;                  /* slots gone: the end, not a defect */
        if (thread_join(tid, 0) != 0)
            churn_bad = 1;
        if (getpid() != me)
            churn_bad = 2;          /* our own slot was recycled under us */
        churn_n++;
    }
    thread_exit(churn_bad);
}

static int probe_create_exit(void)
{
    int tid, i;

    churn_go = 1;
    tid = thread_create(churn_worker, 0, 4096);
    if (tid <= 0) {
        printf("kthreadtest: FAIL create the churn thread\n");
        return 1;
    }
    /* Exit only once the churn is hot, otherwise the leader leaves before
     * the sibling ever runs and the overlap never happens.  200 create/join
     * cycles is ~1 tick each way, so this is a few milliseconds at most. */
    for (i = 0; i < 200 && churn_n < 200 && !churn_bad; i++)
        sleep(1);
    printf("kthreadtest create-exit: exit after %d cycle(s), sibling flagged %d\n",
           churn_n, churn_bad);
    return 4;
}

static volatile int dj_tid;
static volatile int dj_first = -99;
static volatile int dj_second = -99;

static void dj_victim(void *arg)
{
    (void)arg;
    sleep(50);
    thread_exit(42);
}

static void dj_joiner_first(void *arg)
{
    int code = -1;
    int rc = thread_join(dj_tid, &code);

    (void)arg;
    dj_first = rc == 0 ? code : rc;
    thread_exit(0);
}

static void dj_joiner_second(void *arg)
{
    (void)arg;
    dj_second = thread_join(dj_tid, 0);
    thread_exit(0);
}

static int probe_double_join(void)
{
    int t1, t2, i;

    dj_tid = thread_create(dj_victim, 0, 4096);
    ok(dj_tid > 0, "create the thread both siblings will join");
    t1 = thread_create(dj_joiner_first, 0, 4096);
    sleep(10);                      /* the first is parked before the second */
    t2 = thread_create(dj_joiner_second, 0, 4096);
    if (t1 <= 0 || t2 <= 0) {
        printf("kthreadtest: FAIL create the joiners\n");
        return 1;
    }
    /* Bounded: a joiner left in ST_WAIT_THREAD never sets its slot, and a
     * probe that waits forever cannot report what went wrong. */
    for (i = 0; i < 100; i++) {
        if (dj_first != -99 && dj_second != -99)
            break;
        sleep(10);
    }
    printf("kthreadtest: first joiner got %d, second got %d\n",
           dj_first, dj_second);
    ok(dj_first == 42, "first joiner collected the exit code");
    ok(dj_second == -1, "second joiner refused instead of sleeping forever");
    return fails;
}

/* /proc/cpuinfo says whether this port enforces user-memory protection.  The
 * fault probe below writes to FAULT_PROBE_ADDR and expects the thread to die
 * with 139; on a port with no MPU (RP2040) that store is allowed, so the probe
 * would test an assumption the platform cannot satisfy.  Read the capability
 * instead of guessing - and answer "on" when the file or the line is missing,
 * so an unknown port keeps the strict check. */
static int memprot_on(void)
{
    char buf[256];
    char *p;
    int fd, n;

    fd = open("/proc/cpuinfo");
    if (fd < 0)
        return 1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 1;
    buf[n] = 0;
    p = strstr(buf, "memprot");
    if (!p)
        return 1;
    p = strchr(p, ':');
    if (!p)
        return 1;
    while (*++p == ' ' || *p == '\t')
        ;
    return strncmp(p, "off", 3) != 0;
}

int main(int argc, char *argv[])
{
    int code, tid, n, i;
    int tids[40];

    if (argc > 1 && !strcmp(argv[1], "leave")) {
        tid = thread_create(forever, 0, 4096);
        printf("kthreadtest: leader exits, thread %d still alive\n", tid);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "pipes"))
        return probe_pipes();
    if (argc > 1 && !strcmp(argv[1], "create-exit"))
        return probe_create_exit();
    if (argc > 1 && !strcmp(argv[1], "double-join"))
        return probe_double_join();

    code = -1;
    tid = thread_create(give42, 0, 4096);
    ok(tid > 0, "create");
    ok(thread_join(tid, &code) == 0 && code == 42,
       "join returns the exit code");

    ok(thread_join(tid, &code) == -1, "second join fails");

    code = -1;
    tid = thread_create(give42, 0, 4096);
    sleep(100);                 /* let it exit: the slot becomes a zombie */
    ok(thread_join(tid, &code) == 0 && code == 42,
       "join after exit (zombie slot)");

    ok(thread_join(9999, &code) == -1, "join unknown tid fails");

    /* Exhaustion: threads exit immediately but hold their slot until joined,
     * exactly like an unwaited child.  What runs out is table slots, not
     * memory, so the stacks are the API's minimum (512 B): with 4 KiB each
     * the process heap would run out first and the loop would stop at a
     * number that says nothing about the table. */
    n = 0;
    for (i = 0; i < 40; i++) {
        codes[i] = i + 1;
        tid = thread_create(give, &codes[i], 512);
        if (tid < 0)
            break;
        tids[n++] = tid;
    }
    printf("kthreadtest: %d thread slot(s) usable beside shell+self\n", n);
    ok(n > 0 && n < 40, "slot exhaustion returns -1");

    /* A full thread table must not leak a process entry either: exec() claims
     * both, the process first, and has to give that one back when the thread
     * slot is the one that ran out (PROC_NEW -> PROC_FREE).  Without this the
     * process table fills with ghosts and every later exec fails on a machine
     * with no processes on it. */
    ok(exec("/bin/ls", NULL, 0) < 0, "exec fails when the thread table is full");

    for (i = 0; i < n; i++) {
        code = -1;
        if (thread_join(tids[i], &code) != 0 || code != i + 1) {
            fails++;
            printf("kthreadtest: FAIL churn join %d (code %d)\n", i, code);
        }
    }

    tid = thread_create(give42, 0, 4096);
    ok(tid > 0, "create works again after joining everything");

    /* The other half of the same seam: after draining the table, exec() must
     * work again, which it cannot if the failed attempts above left a process
     * entry claimed.  ls is a program of its own on every image; echo moved
     * into /bin/tools with the multi-call merge, so /bin/echo no longer
     * exists. */
    {
        int child = exec("/bin/ls", NULL, 0);

        ok(child > 0, "exec works again after the table drains");
        if (child > 0)
            ok(wait(child) == 0, "the exec'd program ran and exited");
    }

    code = -1;
    ok(thread_join(tid, &code) == 0 && code == 42, "churn join code");

    {
        uint32_t t0, t1;

        /* Deterministic overlap test: the thread sleeps 200 ms and only then
         * sets `woke`; the leader sleeps 50 ms and looks.  If sleep() parked
         * the whole process (as the cooperative library does), the leader
         * could not observe woke == 0.
         *
         * The exit code travels as the integer argument: since thread stacks
         * are private, a pointer to the leader's stack may fault in the
         * thread (docs/REAL-THREADS.md). */
        woke = 0;
        t0 = clock_ms();
        tid = thread_create(slow, (void *)7, 4096);
        sleep(50);
        ok(woke == 0, "leader ran while the thread was still asleep");
        /* The thread slot now has its own stack range (S3b): ps must show a
         * thrN row in sleep with a real stk/sz, not 0/0 or the leader's. */
        printf("kthreadtest: ps while the thread sleeps:\n");
        ps();
        code = -1;
        ok(thread_join(tid, &code) == 0 && code == 7, "join sleeping thread");
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
        /* The fd, not a pointer to it: the thread has its own stack. */
        tid = thread_create(fduser, (void *)fd, 4096);
        sleep(20);
        ok(fd_read_result == 1, "thread read through the shared fd table");
        close(fd);
        code = -1;
        ok(thread_join(tid, &code) == 0, "join the fd thread");
        ok(fd_read_result == -1, "leader close() closed it for the thread");
        (void)junk;
    }

    /* S3b: the stack range now belongs to the thread.  Overflowing it must
     * kill that thread and nobody else; the leader keeps running and join
     * still collects the code (139, the same one a user fault uses). */
    {
        int code = 0;

        tid = thread_create(overflow, (void *)0, 4096);
        ok(tid > 0, "create an overflowing thread");
        code = -1;
        ok(thread_join(tid, &code) == 0 && code == 139,
           "thread stack overflow killed only the thread");
        ok(1, "leader survived the thread stack overflow");
    }

    /* S1: a thread sees the leader's cwd and its chdir moves the process. */
    {
        char buf[TOS_PATH_MAX];
        int code = -1;

        ok(chdir("/bin") == 0, "leader chdir(/bin)");
        tid = thread_create(cwduser, 0, 4096);
        ok(thread_join(tid, &code) == 0 && code == 0,
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
            tid = thread_create(give42, 0, 4096);
            if (tid < 0) {
                failed = i ? i : -1;
                break;
            }
            code = -1;
            if (thread_join(tid, &code) != 0 || code != 42) {
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

    /* The stack area's own reclaim test: 1000 cycles alternating 4 KiB and
     * 16 KiB.  `live` includes the leader and shell stacks, so it is
     * compared with the value before the loop, not with zero; `reserved`
     * must come back to the same size because the 16 KiB block is the step
     * that growth added.  A board without an area prints N/A: there the
     * 16 KiB stacks would come from sbrk, and 1000 of them would exhaust
     * the heap for a reason this test is not about. */
    {
        struct stackarea a = {0, 0, 0, 0}, b = {0, 0, 0, 0};
        int i, code, failed = 0;

        if (stackinfo(&a) != 0) {
            ok(0, "stackinfo");
        } else if (a.reserved == 0) {
            printf("kthreadtest: N/A 1000 mixed create/join (no stack area)\n");
        } else {
            for (i = 0; i < 1000; i++) {
                unsigned bytes = (i & 1) ? 16u * 1024u : 4u * 1024u;

                tid = thread_create(give42, 0, bytes);
                if (tid < 0) {
                    failed = i ? i : -1;
                    break;
                }
                code = -1;
                if (thread_join(tid, &code) != 0 || code != 42) {
                    failed = 1000 + i;
                    break;
                }
            }
            if (stackinfo(&b) != 0)
                failed = -2;
            printf("kthreadtest: 1000 mixed 4K/16K create/join, live %u -> %u, "
                   "reserved %u -> %u, peak %u%s\n",
                   a.live, b.live, a.reserved, b.reserved, b.peak,
                   failed ? " (stopped early)" : "");
            ok(failed == 0, "1000 mixed create/join cycles complete");
            ok(failed != 0 || b.live == a.live,
               "every stack block was returned");
            ok(failed != 0 || b.reserved == a.reserved,
               "the stack area shrank back to its baseline");
        }
    }

    /* Two CPUs, one break: sbrk() must hand out disjoint blocks. */
    {
        int tids[SMP_THREADS];
        int i, code, bad = 0;
        uint32_t end;
        uint64_t got = 0;
        const uint64_t want = (uint64_t)SMP_THREADS * SMP_ITERS;

        smp_go = 0;
        for (i = 0; i < SMP_THREADS; i++) {
            smp_sum[i] = 0;
            smp_err[i] = 0;
            tids[i] = thread_create(smp_sbrk, (void *)i, 4096);
            bad |= tids[i] <= 0;
        }
        /* Every stack exists now, so nothing but the sbrk() calls below can
         * move the break until the phase is over. */
        smp_base = (uint32_t)sbrk(0);
        smp_go = 1;
        for (i = 0; i < SMP_THREADS; i++) {
            code = -1;
            if (tids[i] > 0 && thread_join(tids[i], &code) != 0)
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

    /* Since the stack area, each thread's stack is its own protection
     * region, so a sibling switch programs the unit once - not more.  The
     * count below is the check: at most one set per switch, which is what
     * catches a per-syscall reprogram storm.  (Before the area the stacks
     * shared the heap region and the expected delta was zero.) */
    {
        struct schedstat a, b;
        int ids[YTHREADS];
        uint32_t sw;
        int i, code, bad = 0;

        for (i = 0; i < YTHREADS; i++) {
            yran[i] = 0;
            ids[i] = thread_create(yielder, (void *)i, 4096);
            bad |= ids[i] <= 0;
        }
        ok(!bad, "create yielding threads");
        schedstat(&a);
        for (i = 0; i < YTHREADS; i++) {
            code = -1;
            if (ids[i] > 0 && thread_join(ids[i], &code) != 0)
                bad = 1;
            if (code != 0)
                bad = 1;
        }
        schedstat(&b);
        for (i = 0; i < YTHREADS; i++)
            bad |= yran[i] != YROUNDS;
        ok(!bad, "join yielding threads and all rounds ran");
        sw = b.switches - a.switches;
        ok(b.prot - a.prot <= sw + 2,
           "sibling switches program the regions at most once each");
        printf("kthreadtest: %u sibling switch(es), %u region set(s)\n",
               sw, b.prot - a.prot);
    }

    /* Blocking mutex: sleep while taken, hand off on unlock, and release if
     * the holder dies. */
    {
        int code = -1;

        tmutex = 0;
        locker_got = 0;
        ok(thread_mutex_lock(&tmutex) == 0, "leader took the test mutex");
        tid = thread_create(locker, 0, 4096);
        sleep(50);
        ok(locker_got == 0, "the waiter is blocked, not spinning");
        printf("kthreadtest: ps with a mutex waiter:\n");
        ps();
        ok(thread_mutex_unlock(&tmutex) == 0, "leader released the test mutex");
        ok(thread_join(tid, &code) == 0 && code == 0,
           "the blocked thread ran after the handoff");
        ok(locker_got == 1, "the waiter saw its critical section");
        ok(thread_mutex_try(&tmutex) == 1, "mutex is free after the waiter exited");
        thread_mutex_unlock(&tmutex);
    }

    {
        int code = -1;
        int prot = memprot_on();

        deadmutex = 0;
        tid = thread_create(die_holding, 0, 4096);
        ok(tid > 0, "create a thread that dies holding a mutex");
        if (prot) {
            ok(thread_join(tid, &code) == 0 && code == 139,
               "the holder died on its fault");
        } else {
            /* No MPU/PMP here: the store to the dead address is not a fault,
             * so the thread exits normally.  Say so instead of pretending the
             * probe ran; the half that matters on this port - a thread that
             * goes away while holding a mutex must not pin it - is checked
             * below and is not weakened. */
            ok(thread_join(tid, &code) == 0,
               "join the thread that wrote to a dead address (memprot: off)");
            printf("kthreadtest: N/A the holder died on its fault (memprot: off)\n");
        }
        ok(thread_mutex_try(&deadmutex) == 1, "a dead holder released the mutex");
        thread_mutex_unlock(&deadmutex);
    }

    /* The user heap is shared by the process's threads; the kernel mutex in
     * heap.c has to keep every block intact under concurrent callers. */
    {
        int tids[HEAP_THREADS];
        int i, code, bad = 0;

        for (i = 0; i < HEAP_THREADS; i++) {
            heap_bad[i] = 0;
            tids[i] = thread_create(heaper, (void *)i, 4096);
            bad |= tids[i] <= 0;
        }
        for (i = 0; i < HEAP_THREADS; i++) {
            code = -1;
            if (tids[i] > 0 && thread_join(tids[i], &code) != 0)
                bad = 1;
            if (code != 0)
                bad = 1;
            bad |= heap_bad[i] != 0;
        }
        ok(!bad, "concurrent malloc/free keeps every block intact");
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
