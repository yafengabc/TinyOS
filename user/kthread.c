#ifdef TOS_KTHREADS

/* Kernel-thread library (experimental).
 *
 * The kernel side is a process-table slot that shares the caller's window:
 * kthread_create() hands the kernel an entry point, one argument and the top
 * of a stack, and the scheduler treats the thread like any other task, so it
 * is preempted by the 1 ms tick and a blocking read() parks only that thread.
 *
 * The stack comes from the program heap (sbrk), i.e. from inside the group's
 * window, which is what makes it visible to the shared protection regions.
 * Create every thread before malloc() starts handing out the rest of the
 * heap: the break is shared, so both work, but the stacks are not movable.
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
    char *base;

    if (stack_bytes < 512)
        stack_bytes = 512;
    bytes = (stack_bytes + 31u) & ~15u;

    base = sbrk((int)(bytes + sizeof(struct kstart) + 16));
    if (!base)
        return -1;

    top = ((unsigned)base + bytes) & ~15u;
    s = (struct kstart *)(top - 16);
    s->fn = fn;
    s->arg = arg;

    return tos_sys3(SYS_THREAD_CREATE, (int)kthread_trampoline, (int)s, top);
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
