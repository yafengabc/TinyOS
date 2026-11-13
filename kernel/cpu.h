#ifndef TINYOS_CPU_H
#define TINYOS_CPU_H

#include "types.h"
#include "diag.h"

/* Per-CPU state and the locks that make sharing it safe.
 *
 * The kernel was written for one CPU: `current`, the round-robin cursor and
 * the "slice is over" flag were file-static variables in process.c.  On an
 * SMP build those become per-CPU, because two cores running two different
 * tasks each need their own answer to "who am I running".
 *
 * What stays shared is the process table itself: there is one run queue, and
 * both cores take tasks from it.  That is the part that needs a lock, and it
 * is the only part: memory protection, the trap frame and the kernel stack
 * are already per-CPU by construction.
 *
 * The lock collapses to nothing on a uniprocessor, which is why the single
 * core boards pay no price for any of this. */

struct proc;

/* Bytes of kernel stack per CPU.  The linker reserves
 * `KSTACK_BYTES * PLAT_MAX_CPUS` in .bss (start.S on rv32 and arm, entry.S on
 * x86); process.c uses the same number for the overflow guard.
 *
 * These three used to be kept in step by hand, and they drifted: i386
 * reserved 16 KiB while this said 32 KiB, so the guard word was written 16 KiB
 * below the stack - into the process table - and `ps` listed a phantom task
 * whose `state` was 0x5354414B.  cpu_init() now compares the arena against
 * this constant and panics on a mismatch, which turns a corruption bug into a
 * one-line boot error.
 *
 * ARM and RISC-V take 16 KiB.  ARM needed it first: the RP2350 has 128 KiB
 * for the entire kernel, so two 32 KiB stacks would crowd out everything
 * else.  RISC-V then turned out to want the same number for a different
 * reason - the deepest kernel path is a trap frame plus a syscall, and a
 * recursive user program like `py` recurses on its own stack, so half of a
 * 32 KiB reservation was never touched.  i386 keeps 32 KiB because
 * arch/x86/entry.S reserves exactly that. */
#ifndef TOS_KSTACK_BYTES
#if defined(__arm__) || defined(__thumb__) || defined(__riscv)
#define TOS_KSTACK_BYTES 16384
#else
#define TOS_KSTACK_BYTES 32768
#endif
#endif

#define KSTACK_BYTES ((uint32_t)TOS_KSTACK_BYTES)

/* Where CPU `id`'s kernel stack sits.  The linker lays the blocks out back
 * to back with hart 0's at the top, so this is arithmetic rather than a
 * per-CPU table: a value that has to be initialised is a value that can be
 * read before it is. */
extern uint8_t __kernel_stack[];
extern uint8_t __kernel_stack_top[];
#define CPU_KSTACK_TOP(id) ((uint32_t)__kernel_stack_top - (uint32_t)(id) * KSTACK_BYTES)
#define CPU_KSTACK_LOW(id) (CPU_KSTACK_TOP(id) - KSTACK_BYTES)

/* What the linker actually reserved, for the boot-time check above. */
#define KSTACK_POOL_BYTES ((uint32_t)__kernel_stack_top - (uint32_t)__kernel_stack)

/* Upper bound on the CPU table.  A `const uint32_t` cannot size an array,
 * so this is a macro; four is already more than any board here needs and
 * eight costs 8 * ~24 bytes of .bss. */
#define MAX_CPUS 8

typedef struct cpu {
    int      id;
    int      online;
    /* The task this CPU is running.  Named `cur` rather than `current`
     * because process.c reaches the same field through a macro called
     * `current`, and `cpus[i].current` would then expand twice. */
    struct proc *cur;
    int      quantum;          /* ticks left in this CPU's slice   */
    volatile int need_resched; /* get off this task at the next exit */
    int      rr;               /* round-robin cursor for this CPU  */
} cpu_t;

extern cpu_t cpus[MAX_CPUS];
extern volatile int cpu_count;

/* Which CPU is asking, and its state. */
int  cpu_id(void);
cpu_t *cpu_self(void);

/* Bring-up: called once on the boot CPU after the process table exists,
 * then starts every other CPU. */
void cpu_init(void);

/* Entry point for a secondary CPU, from start.S.  Never returns. */
void cpu_secondary_main(void) __attribute__((noreturn));

/* Take the first runnable task for this CPU, mark it running and return it;
 * NULL when nothing is ready.  Used by a secondary CPU that has no trap
 * frame yet - it starts a task by entering user mode directly. */
struct proc *cpu_take_task(void);

/* ---- locks ----------------------------------------------------------- */

/* A spin lock.  On a uniprocessor both calls compile to nothing: the kernel
 * cannot be switched away from inside a syscall (a tick that lands there
 * returns without rescheduling), so there is nothing to exclude. */
typedef struct {
    volatile int v;
} spin_t;

#define SPIN_INIT { 0 }

void spin_lock(spin_t *s);
void spin_unlock(spin_t *s);

/* Take the lock only if it is free right now; returns 0 when somebody else
 * holds it.  For output paths: a console that drops a line under contention
 * can never wedge a core, while one that spins can (and did - the board hung
 * solid with interrupts off inside a print). */
int  spin_trylock(spin_t *s);

/* The arch/board primitives behind the two calls above.  A port with more
 * than one CPU must provide real atomics here (RV32A amoswap, ARM LDREX/STREX,
 * or the RP2040's SIO spinlock block). */
void plat_spin_acquire(volatile int *v);
void plat_spin_release(volatile int *v);

/* Called from a core's idle loop and timer tick so that another core can park
 * it out of XIP for a ROM flash operation (XIP is in direct mode then, and an
 * instruction fetch from it is a bus fault).  Weak no-op by default; boards
 * with flash programming override it. */
void plat_flash_park_poll(void);

/* Wake a CPU that is parked in plat_cpu_idle().  Weak no-op by default; a
 * port whose idle is wfi and whose wake-up is sev (ARM) overrides it. */
void plat_wake_cpu(void);

#endif
