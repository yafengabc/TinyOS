#include "cpu.h"
#include "platform.h"
#include "process.h"
#include "console.h"

/* Per-CPU state, secondary CPU bring-up, and the two lock primitives.  See
 * cpu.h for why there is so little of it. */

cpu_t cpus[MAX_CPUS];
volatile int cpu_count = 1;

/* Magic the secondary CPUs spin on in start.S.  It lives in .bss, which the
 * boot CPU clears before anything else, and is written only once the
 * scheduler and the timer exist - so a secondary that reads the magic
 * really can touch kernel memory.  A plain flag would race with the .bss
 * clear itself. */
volatile uint32_t cpu_release = 0;
#define CPU_RELEASE_MAGIC 0x534D5031u        /* "SMP1" */

int cpu_id(void)
{
    return plat_cpu_id();
}

cpu_t *cpu_self(void)
{
    return &cpus[plat_cpu_id()];
}

/* Uniprocessors have nothing to arm: plat_timer_init() already programmed
 * the one timer there is.  A board that grows a second hart overrides this
 * (qemu-virt does), which is the same weak-default pattern the GPIO and ADC
 * layers use - the kernel never has to know which boards are SMP. */
__attribute__((weak)) void plat_timer_start_cpu(void)
{
}

void cpu_init(void)
{
    int n = (int)PLAT_MAX_CPUS;

    if (n < 1)
        n = 1;
    if (n > (int)(MAX_CPUS))
        n = (int)(MAX_CPUS);

    /* The linker reserved the stack pool; KSTACK_BYTES divides it.  A
     * mismatch does not fail to link - it puts the overflow guard word
     * somewhere else entirely (see cpu.h) - so it has to be caught here,
     * where the message still makes sense. */
    if (KSTACK_POOL_BYTES != KSTACK_BYTES * (uint32_t)n)
        panic("kernel stack pool is %u bytes, expected %u (%u per CPU)",
              (unsigned)KSTACK_POOL_BYTES, (unsigned)(KSTACK_BYTES * n),
              (unsigned)KSTACK_BYTES);

    for (int i = 0; i < n; i++) {
        cpus[i].id = i;
        cpus[i].online = 0;
        cpus[i].cur = 0;
        cpus[i].quantum = 0;
        cpus[i].need_resched = 0;
        cpus[i].rr = 0;
    }
    cpus[0].online = 1;
    cpu_count = 1;

    if (n > 1) {
        /* Everything the secondaries need exists from here on: the process
         * table (process_init ran), the timer (plat_timer_init ran) and the
         * console.  Release them and give them a moment to appear. */
        cpu_release = CPU_RELEASE_MAGIC;

        for (uint32_t spins = 0; spins < 20000000u; spins++) {
            if (cpu_count == n)
                break;
        }
        kprintf("[smp] %d of %d CPU(s) online\n", cpu_count, n);
    }
}

/* Called on a secondary CPU, from start.S, with its own kernel stack and
 * trap vector already installed.  There is no kmain here: the kernel is up,
 * the only thing missing is somebody to run tasks. */
void cpu_secondary_main(void)
{
    int id = plat_cpu_id();
    cpu_t *me;

    if (id < 0 || id >= (int)(MAX_CPUS))
        plat_panic_halt();          /* no such hart: nothing sane to do */
    me = &cpus[id];

    me->id = id;
    me->cur = 0;
    me->quantum = 0;
    me->online = 1;
    cpu_count++;

    /* This CPU needs its own timer: mtimecmp is per hart, so the one the
     * boot CPU armed says nothing about us. */
    plat_timer_start_cpu();

    plat_irq_enable();

    for (;;) {
        struct proc *p = cpu_take_task();

        if (p) {
            /* First task on this CPU: drop straight into user mode.  From
             * here on every entry is a trap, exactly like the boot CPU. */
            plat_enter_user(process_saved_frame(p));
        }
        plat_irq_enable();
        plat_cpu_idle();
    }
}

/* ---- locks ----------------------------------------------------------- */

/* Weak defaults for a port that has no atomic instruction of its own.  Every
 * board here is a uniprocessor except the qemu-virt SMP build, so "do
 * nothing" is the correct implementation for them - arch/rv32/platform.c
 * overrides both with `amoswap.w.aqrl` when the build is multiprocessor.  It
 * has to be weak rather than absent so that ARMv6-M (no LDREX/STREX) and
 * i386 (no LOCK prefix in this toolchain path) still link. */
__attribute__((weak)) void plat_spin_acquire(volatile int *v)
{
    (void)v;
}

__attribute__((weak)) void plat_spin_release(volatile int *v)
{
    (void)v;
}

/* On a uniprocessor the lock is a no-op in the compiler, not just in the
 * hardware: the kernel is never preempted inside a kernel call (a tick that
 * lands there returns without rescheduling), so there is nothing to exclude.
 * Keeping the test here rather than in each port means a single-hart build
 * of an SMP-capable port still assembles - and, more importantly, that a
 * board which cannot do atomics does not have to fake them. */
void spin_lock(spin_t *s)
{
    if (cpu_count > 1)
        plat_spin_acquire(&s->v);
}

void spin_unlock(spin_t *s)
{
    if (cpu_count > 1)
        plat_spin_release(&s->v);
}
