/* Cooperative threads, user space only.
 *
 * A thread is a stack and a saved sp: switching is a handful of registers
 * pushed by thread_switch.S, and the kernel does not know threads exist.
 * That buys the natural shape - "do this, then wait here" - without
 * touching the scheduler, at the price of one rule: a plain read()/sleep()
 * parks the whole process, so a thread that waits must use thread_read()
 * (poll + yield) or yield itself.  Timer preemption still switches
 * processes, never two threads of one process, which is why mutex_lock is
 * a plain flag and not an atomic.
 *
 * Four threads total, main included.  Stacks come from the program's heap
 * (TOS_HEAP), so a program that never calls thread_create() pays neither
 * the stacks nor the code: the build only links this file when the config
 * has threads = 1, and --gc-sections drops it from programs that do not
 * call it. */

#include "lib.h"

#define THREAD_MAX 4

#define T_FREE  0
#define T_READY 1
#define T_RUN   2
#define T_DONE  3

struct thread {
    uint32_t sp;             /* saved stack pointer while off-CPU */
    void   (*fn)(void *);
    void    *arg;
    int      ret;
    uint8_t  state;
};

static struct thread th[THREAD_MAX];
static int current_tid = -1;

/* thread_switch.S: save callee-saved registers and sp into *save_sp, then
 * restore the same set from new_sp and return into the other thread. */
void thread_switch(uint32_t *save_sp, uint32_t new_sp);

static void thread_entry(void);

static void thread_init(void)
{
    if (current_tid < 0) {
        th[0].state = T_RUN;
        current_tid = 0;
    }
}

/* Round robin, skipping free and finished threads.  T_RUN counts because the
 * main thread sits in that state while it is joining. */
static int pick_next(int me)
{
    for (int i = 1; i <= THREAD_MAX; i++) {
        int n = (me + i) % THREAD_MAX;

        if (n == me)
            continue;
        if (th[n].state == T_READY || th[n].state == T_RUN)
            return n;
    }
    return -1;
}

/* Build the initial frame so the first thread_switch() "returns" into
 * thread_entry.  Layouts match thread_switch.S exactly; the ABI alignment
 * each target expects at a function entry is the reason for the padding. */
static uint32_t thread_frame(uint32_t top, void (*entry)(void))
{
#if defined(__riscv)
    uint32_t *f = (uint32_t *)(top - 64);

    for (int i = 0; i < 16; i++)
        f[i] = 0;
    f[0] = (uint32_t)entry;          /* ra; s0-s11 follow, then padding */
    return (uint32_t)f;
#elif defined(__i386__)
    uint32_t *f = (uint32_t *)(top - 24);

    for (int i = 0; i < 6; i++)
        f[i] = 0;
    f[4] = (uint32_t)entry;          /* popped by the final ret */
    return (uint32_t)f;
#elif defined(__arm__)
    /* The frame is [r8..r11][r4..r7][lr] in ascending memory (two pushes,
     * the second landing lower), so the entry address is the last word.
     * `top - 36` keeps the sp the restore ends on 16-byte aligned. */
    uint32_t *f = (uint32_t *)(top - 36);

    for (int i = 0; i < 9; i++)
        f[i] = 0;
    f[8] = (uint32_t)entry | 1u;     /* lr: Thumb bit */
    return (uint32_t)f;
#else
#error "no thread frame for this architecture"
#endif
}

static void thread_entry(void)
{
    int me = current_tid;

    th[me].fn(th[me].arg);
    thread_exit(0);                  /* a thread that falls off is done */
}

int thread_create(void (*fn)(void *), void *arg, unsigned stack_bytes)
{
    int id;
    unsigned char *base;
    uint32_t top;

    thread_init();
    if (!fn)
        return -1;
    for (id = 1; id < THREAD_MAX; id++)
        if (th[id].state == T_FREE)
            break;
    if (id == THREAD_MAX)
        return -1;
    if (stack_bytes == 0)
        stack_bytes = 4096;
    if (stack_bytes < 512)
        stack_bytes = 512;

    base = malloc(stack_bytes);
    if (!base)
        return -1;                   /* no heap: TOS_HEAP is the answer */

    top = ((uint32_t)base + stack_bytes) & ~15u;
    th[id].fn = fn;
    th[id].arg = arg;
    th[id].ret = 0;
    th[id].state = T_READY;
    th[id].sp = thread_frame(top, thread_entry);
    return id;
}

void thread_exit(int code)
{
    int me, next;

    thread_init();
    me = current_tid;
    th[me].ret = code;
    th[me].state = T_DONE;

    next = pick_next(me);
    if (next < 0)
        exit(code);                  /* last thread: the process is done */

    th[next].state = T_RUN;
    current_tid = next;
    thread_switch(&th[me].sp, th[next].sp);
    exit(code);                      /* a finished thread never resumes */
}

int thread_join(int tid, int *code)
{
    thread_init();
    if (tid <= 0 || tid >= THREAD_MAX || tid == current_tid)
        return -1;
    if (th[tid].state == T_FREE)
        return -1;

    while (th[tid].state != T_DONE) {
        if (!thread_yield())
            return -1;               /* cannot make progress */
    }
    if (code)
        *code = th[tid].ret;
    th[tid].state = T_FREE;
    return 0;
}

int thread_self(void)
{
    thread_init();
    return current_tid;
}

int thread_yield(void)
{
    int prev, next;

    thread_init();
    prev = current_tid;
    next = pick_next(prev);
    if (next < 0)
        return 0;

    th[prev].state = T_READY;
    th[next].state = T_RUN;
    current_tid = next;
    thread_switch(&th[prev].sp, th[next].sp);
    return 1;
}

/* The thread-friendly read: waiting is poll() plus yield, so the other
 * threads keep running.  A plain read() would park the process. */
int thread_read(int fd, void *buf, int len)
{
    struct pollfd p;

    p.fd = fd;
    p.events = POLLIN;
    for (;;) {
        p.revents = 0;
        if (poll(&p, 1, 0) > 0)
            break;
        if (!thread_yield())
            sleep(1);                /* nobody else: park for a tick */
    }
    return read(fd, buf, len);
}

/* Cooperative, so no atomics: two threads of one process are never on a CPU
 * at the same time. */
void mutex_init(mutex_t *m)
{
    m->locked = 0;
}

void mutex_lock(mutex_t *m)
{
    while (m->locked)
        thread_yield();
    m->locked = 1;
}

void mutex_unlock(mutex_t *m)
{
    m->locked = 0;
}
