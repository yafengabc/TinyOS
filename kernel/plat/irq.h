#ifndef TINYOS_PLAT_IRQ_H
#define TINYOS_PLAT_IRQ_H

#include "types.h"
#include "tos.h"

/* ---- timer / interrupts ---------------------------------------------- */

/* Programme the periodic tick.  plat_ticks() must advance at `hz`. */
void     plat_timer_init(uint32_t hz);
uint64_t plat_ticks(void);
uint32_t plat_tick_hz(void);

/* Arm this CPU's own periodic interrupt.  On a uniprocessor this is a no-op
 * (plat_timer_init already did it); on SMP it is what a secondary hart calls
 * before it enters the scheduler, because the timer compare register and the
 * interrupt-enable CSR are per hart - hart 1 inherits nothing from hart 0. */
void plat_timer_start_cpu(void);

/* Per-core processor state that core 0 set during plat_init() and that a
 * secondary core does not inherit: on ARM that is the SCB's exception
 * priorities and its fault-enable bits.  Called from cpu_secondary_main()
 * before the core first enters user mode.  Weak default does nothing, so
 * architectures whose secondary cores need no such setup (riscv, x86) are
 * unaffected. */
void plat_cpu_secondary_init(void);

/* Bring CPU `id` out of reset.  Boards whose secondaries are already
 * spinning on cpu_release (qemu-virt) leave the weak default in place and
 * return 0; boards where the second core sits in a boot-ROM wait loop
 * (RP2350's SIO FIFO) implement it.  Called once per CPU from cpu_init(),
 * after the process table, the timer and the console exist. */
int  plat_start_cpu(int id);

void plat_irq_enable(void);
void plat_irq_disable(void);
void plat_cpu_idle(void);             /* hlt / wfi, interrupts enabled */

/* Save the interrupt-enable state and disable interrupts.  plat_irq_restore()
 * puts back exactly what was there.
 *
 * A critical section that can be entered both from a system call (interrupts
 * on) and from the tick handler (interrupts already off) must not re-enable
 * on the way out, so disable()/enable() are the wrong pair; and on ARM
 * plat_irq_enable() has a side effect - it unmasks the board's tick source
 * the first time it runs on a core.  See docs/OPEN-ITEMS.md §23.3. */
uint32_t plat_irq_save(void);
void     plat_irq_restore(uint32_t state);

/* The arch dispatcher reports an external interrupt as a CPU line number.
 * Exactly one of them is the clock; the rest belong to the board (the C3
 * routes console input to its own line so a byte is drained the moment it
 * arrives instead of at the next tick - ten milliseconds is 115 bytes at
 * 115200, which leaves almost no room in a 128-byte receive FIFO). */
int  plat_irq_is_tick(uint32_t line);
/* Acknowledge a non-tick line after its work is done.  Some sources latch:
 * the UART's receive-timeout bit stays set until written back, and leaving
 * it set means the interrupt fires again the moment we return - the CPU
 * spends all its time in the handler and no task ever runs. */
void plat_irq_service(uint32_t line);

/* ---- cpu topology ----------------------------------------------------- */

/* How many CPUs this build supports.  The build passes -DTOS_MAX_CPUS=2 for
 * the SMP target (qemu-virt with -smp 2) and nothing for the others; the
 * fallback keeps every `#if TOS_MAX_CPUS > 1` in the tree honest without a
 * second macro.  start.S reads the same define, which is how the trap
 * vectors and the kernel-stack blocks stay in step with it. */
#ifndef TOS_MAX_CPUS
#define TOS_MAX_CPUS 1
#endif

/* Compile-time diagnostics.  Driven by `boot_trace` in a config's [build]
 * section (menuconfig: "Boot traces + diagnostics"), together with the
 * platform's boot-progress lines: one switch decides whether an image
 * explains itself.  On by default - the RP2350 acceptance script reads the
 * [A]..[D] markers, and wedge reports are the only way to tell "wrong" from
 * "never ran" on a board with no debugger.  What they add is the ability to
 * answer "which core wedged, inside what, and what was the other one doing"
 * from a serial capture instead of from a debugger (kernel/diag.h). */
#ifndef TOS_DIAG
#define TOS_DIAG 0
#endif

/* Boot-progress traces on the console ([A]/[B]/[C]/[D], the USB stage lines,
 * the CDC configuration notifications).  Set by `boot_trace` in the config
 * ([build] section, menuconfig "Boot traces + diagnostics") and shared with
 * TOS_DIAG above, so one switch moves the whole image between "explains
 * itself" and "quiet". */
#ifndef TOS_BOOT_TRACE
#define TOS_BOOT_TRACE 1
#endif

/* How many CPUs the kernel was built for.  Every board but one is a
 * uniprocessor and sets 1; the SMP build (rv32smp) sets 2 and boots the
 * secondaries from start.S.  This is a compile-time constant, not a runtime
 * probe: with no MMU there is nothing to allocate per CPU at runtime except
 * a kernel stack, and the linker reserves exactly that many.
 *
 * It is a separate symbol from TOS_MAX_CPUS on purpose: the board owns the
 * answer (layout.c), the build only says what it is, and a board that
 * cannot do SMP simply keeps saying 1 whatever the build asked for. */
extern const uint32_t PLAT_MAX_CPUS;

/* Which CPU is asking.  Valid everywhere, including before the scheduler
 * exists (a secondary hart calls kmain-adjacent code with its own id). */
int plat_cpu_id(void);

/* ---- CPU description (for /proc/cpuinfo) ----------------------------- */

/* Core clock in Hz, or 0 when the port does not know it.  This is the CPU's
 * own clock, not the scheduler tick (plat_tick_hz) and not the bus clock. */
uint32_t plat_cpu_hz(void);

/* Scale an old pre-tick spin bound (written for the 125 MHz RP2040) to the
 * board's actual clock.  It is an upper bound, not a delay: callers still
 * poll the state bit and return as soon as it is ready.  A board that has not
 * filled in plat_cpu_hz() yet keeps the old count.
 *
 * 32-bit on purpose: the obvious 64-bit form pulls __udivdi3 into every
 * loop-condition evaluation on RV32.  The clock is rounded up to the next
 * 125 kHz so the result is never smaller than the exact value, and bounds
 * below 1000 iterations are not scaled at all. */
static inline uint32_t plat_pre_tick_spins(uint32_t old_bound)
{
    uint32_t hz = plat_cpu_hz();
    uint32_t per_125k;

    if (hz == 0 || old_bound < 1000u)
        return old_bound;
    per_125k = (hz + 124999u) / 125000u;
    return (old_bound / 1000u) * per_125k;
}

/* Human-readable core and ISA names.  The board owns model (the chip/core
 * combination); the architecture port owns isa because it is a property of
 * how this image was compiled, not of the board.  Both have weak defaults
 * that say "unknown" so a new board only has to fill in what it knows. */
const char *plat_cpu_model(void);
const char *plat_cpu_isa(void);

/* ---- time ------------------------------------------------------------ */

uint32_t plat_uptime_ms(void);

/* Monotonic nanoseconds since boot.  The tick above is what the scheduler
 * counts; this is the clock: it comes from a free-running hardware counter
 * (CLINT mtime on qemu-virt, the TG0 time base on the ESP32-C3, SysTick plus
 * its live down-counter on Cortex-M) and has sub-tick resolution, so a
 * program can time something shorter than the 10 ms quantum.
 *
 * Boards that only have the tick fall back to `plat_ticks() * period`, which
 * is monotonic too - just coarser.  The value never goes backwards, even if
 * the underlying counter is reloaded while the tick is being serviced. */
uint64_t plat_now_ns(void);

#endif /* TINYOS_PLAT_IRQ_H */
