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

/* How a secondary CPU is brought out of reset.  Two models exist here:
 *
 *   - the secondaries are already running and wait for `cpu_release`
 *     (qemu-virt: every hart starts at the reset vector and start.S parks
 *     the ones that are not hart 0).  Those boards do not override this.
 *   - the secondary sits in a boot-ROM wait loop and has to be kicked, with
 *     a stack and an entry point (RP2350: the SIO FIFO launch).  Those
 *     boards implement it and return 1 on success.
 *
 * Returning 0 is not an error - it means "this board starts its own". */
__attribute__((weak)) int plat_start_cpu(int id)
{
    (void)id;
    return 0;
}

/* Poke a CPU that may be sitting in its idle wait.  On ARM that is `sev`
 * (the counterpart of the `wfi` in plat_cpu_idle); a port whose idle is a
 * real instruction with its own wake-up (rv32's wfi, woken by the timer)
 * needs nothing here, and the weak default keeps every board building. */
__attribute__((weak)) void plat_wake_cpu(void)
{
}

/* CPU description hooks.  A board that knows its core clock and model
 * overrides these; the architecture ports override the ISA string.  The
 * weak defaults keep /proc/cpuinfo usable (and every target building)
 * before a board has filled them in. */
__attribute__((weak)) uint32_t plat_cpu_hz(void)
{
    return 0;
}

__attribute__((weak)) const char *plat_cpu_model(void)
{
    return "unknown";
}

__attribute__((weak)) const char *plat_cpu_isa(void)
{
    return "unknown";
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
         * console.  Release them and give them a moment to appear.
         *
         * The magic is for the ports whose secondaries are already spinning
         * on it (rv32/qemu-virt).  A board whose cores have to be launched
         * gets the kick loop below as well; plat_start_cpu() is a no-op
         * there and returns 0. */
        cpu_release = CPU_RELEASE_MAGIC;

        for (int i = 1; i < n; i++)
            (void)plat_start_cpu(i);

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

__attribute__((weak)) int plat_spin_try_acquire(volatile int *v)
{
    (void)v;
    return 1;              /* one CPU: the lock is never contended */
}

int spin_trylock(spin_t *s)
{
    if (cpu_count <= 1)
        return 1;
    return plat_spin_try_acquire(&s->v);
}
