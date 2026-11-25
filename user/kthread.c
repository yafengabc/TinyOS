#ifdef TOS_KTHREADS

/* Kernel-thread library (experimental).
 *
 * The kernel side is a process-table slot that shares the caller's window:
 * kthread_create() hands the kernel an entry point, one argument and the top
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

/* First code to run on the new stack: call the user function, then exit
 * through the kernel.  Keeping this in user space means the kernel only has
 * to know "entry, arg, sp" - no per-architecture return-address fixing. */
static void kthread_trampoline(struct kstart *s)
{
    void (*fn)(void *) = s->fn;
    void *arg = s->arg;

    fn(arg);
    kthread_exit(0);
}

int kthread_create(void (*fn)(void *), void *arg, unsigned stack_bytes)
{
    unsigned bytes, top;
    struct kstart *s;
    struct tos_thread_stack st;
    char *base;

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

    st.top = top;
    st.size = bytes;
    return tos_sys3(SYS_THREAD_CREATE, (int)kthread_trampoline, (int)s,
                    (int)&st);
}

int kthread_join(int tid, int *code)
{
    return tos_sys2(SYS_THREAD_JOIN, tid, (int)code);
}

void kthread_exit(int code)
{
    tos_sys1(SYS_THREAD_EXIT, code);
    for (;;)
        ;
}

#endif /* TOS_KTHREADS */
