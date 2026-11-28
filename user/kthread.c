#ifdef TOS_KTHREADS

/* Kernel-thread library (experimental).
 *
 * The kernel side is a process-table slot that shares the caller's window:
 * thread_create() hands the kernel an entry point, one argument and the top
 * of a stack, and the scheduler treats the thread like any other task, so it
 * is preempted by the 1 ms tick and a blocking read() parks only that thread.
 *
 * The stack comes from the program heap (malloc), i.e. from inside the
 * group's window, which is what makes it visible to the shared protection
 * regions.  It uses the same free list as the program's own allocations, so
 * there is no ordering rule between thread creation and malloc: a stack is
 * just a block that nobody frees while the thread runs.
 */

#include "lib.h"
#include "usys.h"       /* tos_sys0/1/2/3: arch-specific ecall/int 0x80 */

struct kstart {
    void (*fn)(void *);
    void *arg;
};

/* The stack and the start record are the library's allocation; the kernel
 * only ever sees the stack top.  join() is therefore the only place that can
 * give them back, and it needs to find them from the tid alone: one entry per
 * thread this process created.  Sized above the largest thread table (32), so
 * the kernel table cannot fill before this bookkeeping does.  A thread nobody
 * joins keeps its entry and its stack until the process exits - the same
 * lifetime its kernel slot has.
 *
 * Not thread-safe: two threads calling thread_create()/join() at the same
 * time race on this table, like the rest of the user-space library. */
#define KT_TRACK 40

static struct {
    int tid;
    char *base;
    struct kstart *rec;
} kt[KT_TRACK];

/* First code to run on the new stack: call the user function, then exit
 * through the kernel.  Keeping this in user space means the kernel only has
 * to know "entry, arg, sp" - no per-architecture return-address fixing. */
static void kthread_trampoline(struct kstart *s)
{
    void (*fn)(void *) = s->fn;
    void *arg = s->arg;

    fn(arg);
    thread_exit(0);
}

int thread_create(void (*fn)(void *), void *arg, unsigned stack_bytes)
{
    unsigned bytes, top;
    struct kstart *s;
    struct tos_thread_stack st;
    char *base;
    int tid, slot = -1;

    if (stack_bytes < 512)
        stack_bytes = 512;
    bytes = (stack_bytes + 31u) & ~15u;

    /* 32 bytes of slack below the usable stack: the kernel writes the guard
     * word at top-size-4 and checks it on every syscall.  malloc only
     * promises 8-byte alignment, so the top is aligned here and the slack
     * keeps the guard word inside the block. */
    base = malloc(bytes + 32u);
    if (!base)
        return -1;

    /* The start record lives in the heap, not on the new stack: ARM pushes a
     * 32-byte hardware exception frame just below the initial sp, so anything
     * parked at the top of the stack would be overwritten before the first
     * instruction runs. */
    s = (struct kstart *)malloc(sizeof(*s));
    if (!s) {
        free(base);
        return -1;
    }

    top = ((unsigned)base + bytes + 32u) & ~15u;
    s->fn = fn;
    s->arg = arg;

    for (int i = 0; i < KT_TRACK; i++) {
        if (kt[i].tid == 0) {
            slot = i;
            break;
        }
    }

    st.top = top;
    st.size = bytes;
    tid = tos_sys3(SYS_THREAD_CREATE, (int)kthread_trampoline, (int)s,
                   (int)&st);
    if (tid <= 0) {
        /* No slot, no table entry, bad stack: the kernel did not take the
         * blocks, so they are ours to return. */
        free(s);
        free(base);
        return -1;
    }
    if (slot >= 0) {
        kt[slot].tid = tid;
        kt[slot].base = base;
        kt[slot].rec = s;
    }
    /* else: more threads than KT_TRACK.  Creation itself succeeded, so the
     * thread runs; only the bookkeeping (and later the free) is missing. */
    return tid;
}

int thread_join(int tid, int *code)
{
    int rc = tos_sys2(SYS_THREAD_JOIN, tid, (int)code);

    /* The slot is retired before join returns, so nothing can still be
     * running on the stack.  Free after the call, not before: the kernel
     * writes the exit code through `code` on the way out. */
    if (rc == 0) {
        for (int i = 0; i < KT_TRACK; i++) {
            if (kt[i].tid == tid) {
                free(kt[i].rec);
                free(kt[i].base);
                kt[i].tid = 0;
                break;
            }
        }
    }
    return rc;
}

void thread_exit(int code)
{
    tos_sys1(SYS_THREAD_EXIT, code);
    for (;;)
        ;
}

#endif /* TOS_KTHREADS */
