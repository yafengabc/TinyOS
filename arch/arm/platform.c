#include "platform.h"
#include "board.h"
#include "scs.h"
#include "regs.h"
#include "console.h"
#include "string.h"
#include "process.h"
#include "cpu.h"

#ifdef TOS_SYSCALL_TRACE
/* Implemented in kernel/syscall.c, only on a bring-up build. */
void sys_trace_note(uint32_t v);
#endif

/* Generic ARM (Cortex-M) platform layer, shared by every ARM board.
 *
 * What lives here is identical across boards: exception dispatch, user
 * mode via the Thread/PSP split, interrupt enable/disable, the panic halt.
 * What differs - console, timer, block device, shutdown - belongs to the
 * board and is reached through the hooks in board.h.
 *
 * There is no MMU and no MPU-backed virtual memory: all addresses are
 * physical and task windows come from the bump allocator in
 * kernel/process.c.  User tasks run in Thread mode on PSP and
 * *unprivileged* (CONTROL.nPRIV = 1), with the MPU letting them reach their
 * own window and nothing else.  The kernel runs in Handler mode on MSP,
 * where the MPU's PRIVDEFENA keeps the whole flat address space available.
 *
 * nPRIV is written in arm_resume, still inside the exception: the bit is
 * ignored in Handler mode, so handlers and any interrupt taken on the way
 * out stay privileged, and only the Thread context the exception return
 * installs is unprivileged.  (A PendSV switch is not needed for this; it
 * is what a deferred context switch would use.)
 */

/* ---- faults ----------------------------------------------------------- */

static int frame_is_user(const regs_t *f)
{
    /* v7-M EXC_RETURN: 0xFFFFFFF1 = Handler+MSP, 0xFFFFFFF9 = Thread+MSP,
     * 0xFFFFFFFD = Thread+PSP.  Bit 2 is SPSEL, so this is true exactly for
     * frames that interrupted a user task - the kernel's own Thread-mode
     * launch path (cpu_take_task -> arm_enter_user) runs on MSP and reads
     * as kernel, which is what keeps a tick landing there from consuming
     * need_resched (OPEN-ITEMS §22/§24.4).  (regs.h used to call bit 2
     * "Thread mode"; it is the stack select.) */
    return (f->exc_return & ARM_EXC_RET_S_BIT) != 0;
}

static const char *vec_name(uint32_t vec)
{
    switch (vec) {
    case 0:  return "exception";
    case 2:  return "NMI";
    case 3:  return "HardFault";
    case 4:  return "MemManage";
    case 5:  return "BusFault";
    case 6:  return "UsageFault";
    case 11: return "SVCall";
    case 14: return "PendSV";
    case 15: return "SysTick";
    default: return "exception";
    }
}

/* Data address a MemManage/BusFault refers to, when the status register
 * says it is valid.  A fault on an instruction fetch leaves it empty.
 *
 * On the Cortex-M0+ (ARMv6-M) there is only HardFault: no CFSR, no
 * MMFAR/BFAR, and no configurable fault status at all, so there is nothing
 * to decode - the fault message just reports the PC. */
#ifndef TOS_ARM_M0P
static uint32_t fault_addr(const regs_t *f)
{
    uint32_t cfsr = mmio_read32(ARM_CFSR);

    if (f->vec == 4 && (cfsr & ARM_CFSR_MMARVALID))
        return mmio_read32(ARM_MMFAR);
    if (f->vec == 5 && (cfsr & ARM_CFSR_BFARVALID))
        return mmio_read32(ARM_BFAR);
    return 0;
}
#else
static uint32_t fault_addr(const regs_t *f)
{
    (void)f;
    return 0;
}
#endif

/* Called from every fault stub.  Returns the frame to resume with. */
regs_t *arm_handle_fault(regs_t *f)
{
    if (frame_is_user(f)) {
        uint32_t addr = fault_addr(f);
#ifdef TOS_SYSCALL_TRACE
        sys_trace_note(0x4600u | (uint32_t)f->vec);
#endif
        kprintf("\n[pid %d] %s at 0x%08x", process_current_pid(),
                vec_name(f->vec), f->pc);
        if (addr)
            kprintf(" (address 0x%08x)", addr);
        kprintf("\n");
#ifndef TOS_ARM_M0P
        /* On a real board the exception name alone is not enough: CFSR says
         * whether this was a permission fault, a bad bus access or a bad
         * instruction, and BFAR/MMFAR name the address.  Keep it for user
         * faults too - the first task on a new port almost always dies here.
         *
         * Read once, clear, then print: CFSR/HFSR/SFSR are write-one-to-
         * clear and nothing else in the kernel clears them, so without this
         * the *second* task to die reports the first one's status.  Measured
         * by running the four `prot` cases back to back: all four printed
         * mmfar=0x2002c167, the address only the first one had touched -
         * MMARVALID stays set, so the stale value looks authoritative. */
        {
            uint32_t cfsr  = mmio_read32(ARM_CFSR);
            uint32_t hfsr  = mmio_read32(ARM_HFSR);
            uint32_t mmfar = mmio_read32(ARM_MMFAR);
            uint32_t bfar  = mmio_read32(ARM_BFAR);
            uint32_t sfsr  = mmio_read32(ARM_SFSR);
            uint32_t sfar  = mmio_read32(ARM_SFAR);

            mmio_write32(ARM_CFSR, cfsr);
            mmio_write32(ARM_HFSR, hfsr);
            mmio_write32(ARM_SFSR, sfsr);

            kprintf("  cfsr=0x%08x hfsr=0x%08x mmfar=0x%08x bfar=0x%08x\n",
                    cfsr, hfsr, mmfar, bfar);

            /* A stack that runs past its bottom takes the exception frame
               with it: the push is the first access to fail, so pc/lr in the
               dump are meaningless - say so instead of letting the reader
               chase a fault at address 0.  (The ARM ports have no stack
               growth; rv32's process_stack_grow() does.)  Printed here, not
               at the end, because the console drops whatever does not fit
               in the CDC ring: the tail of a long dump is the first thing
               to go missing. */
            if (cfsr & (ARM_CFSR_MSTKERR | ARM_CFSR_MUNSTKERR)) {
                uint32_t psp;

                __asm__ volatile("mrs %0, psp" : "=r"(psp));
                kprintf("  (stacking error: sp 0x%08x left the stack - stack "
                        "overrun, not a wild pointer)\n", (unsigned)psp);
            }

            kprintf("  sfsr=0x%08x sfar=0x%08x sau_ctrl=0x%08x sau_type=0x%08x\n",
                    sfsr, sfar, mmio_read32(ARM_SAU_CTRL),
                    mmio_read32(ARM_SAU_TYPE));
            kprintf("  aircr=0x%08x shcsr=0x%08x\n",
                    mmio_read32(ARM_AIRCR), mmio_read32(ARM_SHCSR));
            kprintf("  shpr1=0x%08x shpr2=0x%08x shpr3=0x%08x\n",
                    mmio_read32(ARM_SHPR1), mmio_read32(ARM_SHPR2),
                    mmio_read32(ARM_SHPR3));
            {
                /* FAULTMASK/BASEPRI/PRIMASK together decide what may still
                 * preempt this handler.  A fault taken with FAULTMASK set
                 * masks everything but NMI, which is exactly how a handler
                 * that ends in sched_idle()'s WFI can park a core for good
                 * while SysTick stays pending. */
                uint32_t faultmask, basepri;
                __asm__ volatile("mrs %0, faultmask" : "=r"(faultmask));
                __asm__ volatile("mrs %0, basepri" : "=r"(basepri));
                kprintf("  faultmask=0x%08x basepri=0x%08x systick_csr=0x%08x\n",
                        faultmask, basepri,
                        mmio_read32(ARM_SCS_BASE + 0x010u));
            }

            /* ICSR names the exception that was active when this one was
               taken; UFSR catches INVPC/INVSTATE, the two escalation causes
               CFSR's "FORCED-only" HardFault hides.  The raw hardware frame
               checks that regs_t was built from the right stack: if these
               eight words disagree with the fields below, the frame pointer
               (not the user program) is what is broken. */
            kprintf("  icsr=0x%08x ufsr=0x%04x frame=0x%08x\n",
                    mmio_read32(ARM_SCS_BASE + 0xD04u),
                    (unsigned)(cfsr >> 16), (unsigned)(uintptr_t)f);
            const uint32_t *hwp = (const uint32_t *)(uintptr_t)f->hw;
            kprintf("  hwords:");
            for (int i = 0; i < 8; i++)
                kprintf(" %08x", (unsigned)hwp[i]);
            kprintf("\n");
        }
#endif
        kprintf("  lr=0x%08x xpsr=0x%08x hw=0x%08x exc=0x%08x\n",
                f->lr, f->xpsr, f->hw, f->exc_return);
        kprintf("  r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x\n",
                f->r0, f->r1, f->r2, f->r3);
        /* r4-r11 are not part of the hardware frame: arm_trap_enter copies
         * them out of the live registers, so they are as accurate as r0-r3. */
        kprintf("  r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x\n",
                f->r4, f->r5, f->r6, f->r7);
        kprintf("  r8=0x%08x r9=0x%08x r10=0x%08x r11=0x%08x r12=0x%08x\n",
                f->r8, f->r9, f->r10, f->r11, f->r12);
        {
            /* The exact bytes at the faulting PC settle "bad instruction"
             * vs "bad data access" without a debugger. */
            const uint16_t *insn = (const uint16_t *)(uintptr_t)(f->pc & ~1u);
            kprintf("  code@pc:");
            for (int i = 0; i < 8; i++)
                kprintf(" %04x", (unsigned)insn[i]);
            kprintf("\n");
        }
        {
            uint32_t ctrl, psp, msp, primask;
            __asm__ volatile("mrs %0, control" : "=r"(ctrl));
            __asm__ volatile("mrs %0, psp" : "=r"(psp));
            __asm__ volatile("mrs %0, msp" : "=r"(msp));
            __asm__ volatile("mrs %0, primask" : "=r"(primask));
            kprintf("  control=0x%08x psp=0x%08x msp=0x%08x\n",
                    ctrl, psp, msp);
            /* An SVC taken with PRIMASK=1 is escalated to HardFault with
               CFSR completely clear.  Exception entry does not save
               PRIMASK, so this is the value user code was running with.
               (BASEPRI is deliberately not read: ARMv6-M has no such
               register and this diagnostic builds for the M0+ too.) */
            kprintf("  primask=0x%08x\n", primask);
        }
#ifdef TOS_SYSCALL_TRACE
        {
            extern uint32_t g_sys_trace[16];
            extern uint32_t g_sys_trace_i;
            kprintf("  trace:");
            for (int i = 0; i < 16; i++)
                kprintf(" %04x", (unsigned)g_sys_trace[(g_sys_trace_i + (uint32_t)i) & 15u]);
            kprintf("\n");
        }
#endif
        return process_exit(f, 139);
    }

    kprintf("\n*** ARM EXCEPTION ***\n");
    kprintf("%s: pc=0x%08x lr=0x%08x xpsr=0x%08x\n",
            vec_name(f->vec), f->pc, f->lr, f->xpsr);
    kprintf("r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x\n",
            f->r0, f->r1, f->r2, f->r3);
    kprintf("hw=0x%08x exc=0x%08x sp=0x%08x\n", f->hw, f->exc_return, f->kern_sp);
#ifndef TOS_ARM_M0P
    /* A kernel fault needs the same status the user path prints: without it
     * "HardFault at <addr>" is all a dead board says, and the cause (masked
     * SVC? invalid state after a bad exception return? IACCVIOL?) is exactly
     * what the dump is for. */
    kprintf("cfsr=0x%08x hfsr=0x%08x mmfar=0x%08x bfar=0x%08x cpu=%d\n",
            mmio_read32(ARM_CFSR), mmio_read32(ARM_HFSR),
            mmio_read32(ARM_MMFAR), mmio_read32(ARM_BFAR), plat_cpu_id());
#endif
    plat_panic_halt();
    return f;
}

/* Called from the SysTick stub. */
regs_t *arm_handle_tick(regs_t *f)
{
    int from_user = frame_is_user(f);

#ifdef TOS_SYSCALL_TRACE
    sys_trace_note(from_user ? 0x54u : 0x48u);
#endif
    plat_timer_tick();
    process_tick(f);

    /* A tick nested inside an SVCall handler must never switch tasks.  The
       SVCall below it is still active, so an arm_resume() carrying a
       "return to Thread mode" EXC_RETURN would unwind two active exceptions
       at once.  ARMv8-M escalates that to HardFault with HFSR.FORCED set
       and CFSR clear - exactly the RP2350 shell crash.  process_tick()
       already refuses to *raise* need_resched on a non-user frame; it must
       also refuse to *consume* an outstanding request here, leaving it for
       the next trap that really did interrupt user code. */
    if (!from_user)
        return f;

    /* An SVC is *pended*, not taken, when a higher-priority exception is
     * pending at the same instruction boundary - and SysTick (0x00)
     * outranks SVCall (0x40) here.  The stacked frame is Thread+PSP, so
     * the test above reads as "user", but the instruction that has not run
     * yet is an SVC: switching tasks now leaves it pending, the pending
     * SVC is tail-chained against whichever task we switch to, and the
     * saved user frame still holds the pre-svc registers (r0 = syscall
     * number).  Measured on mps2 as the lost syscall result, duplicated
     * 128-byte blocks and the `piperace` EPIPE storm (OPEN-ITEMS §24.4).
     * Let the tick return; the SVC runs at once on the original frame. */
    {
        uint32_t icsr = mmio_read32(ARM_ICSR);

        if ((icsr & ARM_ICSR_PENDSVSET) ||
            ((icsr & ARM_ICSR_VECTPENDING) == (ARM_VEC_SVCALL << 12)))
            return f;
    }

    return process_resched(f);
}

/* ---- boot ------------------------------------------------------------ */

void plat_early_init(void)
{
    plat_board_early();
}

int plat_cpu_id(void)
{
    return (int)plat_board_cpu_id();
}

void plat_init(void)
{
    /* SysTick must outrank SVCall.  A task that blocks inside a syscall
       idles in sched_idle() with SVCall still marked active (there is no
       idle task), and it is the tick that hands over console input and
       wakes sleepers.  At equal priority one exception cannot preempt
       another, so that WFI would park forever with ICSR showing SVCall
       active and SysTick pending - the shell prompt appears and then the
       system goes silent, receiving not a single keystroke.  Raising the
       tick above SVCall also keeps the kernel safe: process_tick() only
       reschedules when the interrupted frame is a user frame, so a tick
       that lands in the middle of a kernel call just returns.

       PendSV is parked at the lowest priority for a future deferred
       context switch. */
    mmio_write32(ARM_SHPR3, (ARM_PRIO_SYSTICK << 24) | (ARM_PRIO_PENDSV << 16));
    mmio_write32(ARM_SHPR2, ARM_PRIO_SVC << 24);
#ifndef TOS_ARM_M0P
    /* Faults sit *below* SysTick so the tick can always preempt a fault
     * handler.  A user fault kills the task from inside the exception -
     * process_exit() ends in sched_switch() and, when nothing else is
     * runnable, in sched_idle()'s WFI.  WFI only wakes on an exception of
     * equal or higher priority than the one that is active; MemManage is
     * 0x00 by reset, so a tick at 0x00 could not interrupt it, and a fault
     * that left no runnable task parked the machine for good (measured on
     * the RP2350: `run prot heap &` went silent after the fault report).
     * At 0x40 the tick outranks a fault handler just as it outranks SVC. */
    mmio_write32(ARM_SHPR1, ARM_PRIO_FAULT << 24 |   /* UsageFault  */
                            ARM_PRIO_FAULT << 16 |   /* BusFault    */
                            ARM_PRIO_FAULT << 8);    /* MemManage   */
#endif

#ifndef TOS_ARM_M0P
    /* Turn the configurable faults on: without these bits an MPU violation
       is escalated to HardFault and `prot` cannot say why the task died.
       The Cortex-M0+ has no configurable fault status (everything is
       HardFault), so this is M3-and-up only.  The Cortex-M33 has the same
       CFSR/MMFAR/BFAR layout, so it gets the full fault report. */
    mmio_write32(ARM_SHCSR, ARM_SHCSR_MEMFAULTENA |
                            ARM_SHCSR_BUSFAULTENA |
                            ARM_SHCSR_USGFAULTENA);
#endif

#if !defined(TOS_ARM_M0P) && !defined(TOS_ARM_NO_MPU)
    /* Nothing here touches the MPU or its MAIR.  Both are programmed by the
       first plat_task_protect(), because until then the region set - and the
       attribute bytes those regions index - belong to the boot ROM, and
       rewriting MAIR underneath it changes what its own regions mean.
       (Measured on the RP2350: writing MAIR0 here was enough to wedge the
       board after the ramdisk test, with the MPU still disabled.)
       The first plat_task_protect() is what switches it on.  Boot runs with
       it off (nothing user-mode exists yet), and an MPU that is on before
       its first region set is a good way to turn a mistake in that set into
       a boot-time hang with no console output.  Switching it on at the first
       context switch means user mode still can never start unprotected,
       while everything that happens before - the banner, the first exec -
       runs on the default map and reports its own failures.  (Measured on
       the RP2350: enabling it here wedged the board silently between the
       ramdisk test and the banner.)
       The Cortex-M0+ (RP2040) has no MPU and keeps the flat model. */
#endif

    plat_board_init();
}

/* ---- interrupts ------------------------------------------------------ */

/* Per CPU: "have this core's tick source been unmasked" is a per-core fact
 * (PRIMASK and SysTick's TICKINT are per-core registers).  A plain static
 * here meant a secondary core saw core 0's "already done" and never enabled
 * its own tick - it would sit in the scheduler loop and simply never be
 * preempted. */
static int irq_ready[MAX_CPUS];

void plat_irq_enable(void)
{
    int id = plat_cpu_id();

    if (id < 0 || id >= (int)MAX_CPUS)
        id = 0;
    /* The boards leave their tick source masked until the scheduler is up;
       this is the first safe moment to unmask it. */
    if (!irq_ready[id]) {
        irq_ready[id] = 1;
        plat_timer_irq_unmask();
    }
    __asm__ volatile("cpsie i" ::: "memory");
}

void plat_irq_disable(void)
{
    __asm__ volatile("cpsid i" ::: "memory");
}

/* PRIMASK is 1 when interrupts are masked, so returning it verbatim is the
 * whole state.  Restored with MSR rather than CPSIE: plat_irq_enable()
 * unmasks the board's tick source on first use per core and this must not. */
uint32_t plat_irq_save(void)
{
    uint32_t pm;

    __asm__ volatile("mrs %0, primask\n\t"
                     "cpsid i"
                     : "=r"(pm)
                     :
                     : "memory");
    return pm;
}

void plat_irq_restore(uint32_t state)
{
    /* MSR PRIMASK needs a context synchronisation event before the next
     * instruction may rely on the new value. */
    __asm__ volatile("msr primask, %0\n\t"
                     "isb"
                     :
                     : "r"(state)
                     : "memory");
}

void plat_cpu_idle(void)
{
    /* Idle as a poll, not a WFI.
     *
     * WFI is what an idle core should use and it works for the ordinary
     * case (the shell blocked in read(), tick arriving every millisecond).
     * It does not survive the MemManage handler that `run prot heap &`
     * produces: the core parks and never wakes, with SysTick pending
     * (ICSR PENDSTSET=1, VECTPENDING=15), SysTick at priority 0x00 against
     * MemManage's 0x40, and PRIMASK/BASEPRI/FAULTMASK all zero.  The same
     * core polling this spot instead keeps receiving ticks - measured by
     * counting plat_ticks() across the wait - so the timer, its delivery
     * and the priorities are sound; only the wake-up is lost.
     *
     * Until that is understood, sleeping is not worth a machine that can
     * silently stop.  The scheduler re-checks the run queue between calls,
     * and sched_idle() also pumps the console, so a missed tick costs one
     * pass instead of the system.  The cost is a core that stays busy while
     * idle - the honest trade against a hang; see docs/OPEN-ITEMS.md. */
    __asm__ volatile("cpsie i" ::: "memory");
}

/* Counterpart of the WFI above: the scheduler wakes the other core after
 * publishing a task so that its next idle-loop pass can take it. */
void plat_wake_cpu(void)
{
    __asm__ volatile("sev" ::: "memory");
}

/* ---- spin locks -------------------------------------------------------
 *
 * Cortex-M has LDREX/STREX (ARMv7-M and ARMv8-M) but no atomic
 * read-modify-write, so the lock is the load-exclusive/store-exclusive
 * retry loop every ARM spinlock is.  A failed STREX - because the other
 * core wrote the word between our LDREX and our STREX - means we never
 * held it, so retrying is the whole algorithm; the LDREX at the top of the
 * next pass is also what clears a stale exclusive tag, so no CLREX is
 * needed on that path.
 *
 * The Cortex-M0+ has neither instruction (ARMv6-M), which is why the RP2040
 * port cannot be SMP at all and this compiles out; the weak no-op defaults
 * in kernel/cpu.c are what it links against. */
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1 && !defined(TOS_ARM_M0P)

#if !defined(TOS_ARM_SIO_SPIN)

static inline uint32_t arm_ldrex(volatile int *p)
{
    uint32_t v;

    __asm__ volatile("ldrex %0, [%1]" : "=r"(v) : "r"(p) : "memory");
    return v;
}

static inline uint32_t arm_strex(uint32_t val, volatile int *p)
{
    uint32_t res;

    __asm__ volatile("strex %0, %1, [%2]"
                     : "=&r"(res) : "r"(val), "r"(p) : "memory");
    return res;
}

#endif /* !TOS_ARM_SIO_SPIN */

/* Raw console line, used when the normal path may itself be stuck.  The
 * board's putc takes no lock (it writes the UART and the USB ring directly),
 * so this works even if out_lock is the lock we are spinning on. */
static void raw_puts(const char *s)
{
    while (*s)
        plat_console_putc(*s++);
}

static void raw_puthex(uint32_t v)
{
    static const char digits[] = "0123456789abcdef";

    raw_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        plat_console_putc(digits[(v >> shift) & 0xfu]);
}

static void raw_putdec(uint32_t v)
{
    char tmp[12];
    int i = 0;

    do {
        tmp[i++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v > 0u && i < (int)sizeof(tmp));
    while (i > 0)
        plat_console_putc(tmp[--i]);
}

/* How long a lock may stay taken before we call it stuck.  At 150 MHz a
 * pass through the acquire loop is a handful of cycles, so this is
 * seconds: long enough that no legitimate holder (a flash erase is 20 ms,
 * the longest thing the kernel does under a lock) trips it. */
#define SPIN_LIMIT 40000000u

/* The Cortex-M33 cores of the RP2350 share no global exclusive monitor:
 * LDREX/STREX only excludes against the local core, so two cores could both
 * "acquire" the same word - measured on real hardware as proc_lock held by
 * both sides at once, with two cores running one task (OPEN-ITEMS §24.4).
 * This is why the SIO block carries 32 real hardware spinlocks: reading
 * SPINLOCK[n] claims it atomically (bit 31 set means we own it now) and
 * writing it releases, with no cache or exclusive-monitor involvement.
 *
 * The gate protects only the flag's test-and-set - never a whole critical
 * section - so the nested-lock shape of the kernel (proc_lock inside
 * heap_lock, a pipe lock inside proc_lock, ...) cannot self-deadlock on
 * one hardware lock.  The gate window is three instructions and is IRQ
 * masked, so a tick landing inside it cannot try to pass it again. */
#if defined(TOS_ARM_SIO_SPIN)

#define SIO_BASE       0xD0000000u
#define SIO_SPINLOCK0  (SIO_BASE + 0x100u)
/* Kernel has no other SIO-spinlock user; pick one far from boot ROM's. */
#define TOS_SIO_GATE   31u

static inline int sio_gate_claim(void)
{
    return (int)(mmio_read32(SIO_SPINLOCK0 + 4u * TOS_SIO_GATE) >> 31);
}

static inline void sio_gate_free(void)
{
    mmio_write32(SIO_SPINLOCK0 + 4u * TOS_SIO_GATE, 0);
}

static inline uint32_t sio_gate_irq_save(void)
{
    uint32_t pm;

    __asm__ volatile("mrs %0, primask\n"
                     "cpsid i" : "=r"(pm) :: "memory");
    return pm;
}

static inline void sio_gate_irq_restore(uint32_t pm)
{
    __asm__ volatile("msr primask, %0" :: "r"(pm) : "memory");
}

#endif /* TOS_ARM_SIO_SPIN */

void plat_spin_acquire(volatile int *v)
{
    uint32_t spins = 0;

    for (;;) {
        while (*v != 0) {
            if (++spins == SPIN_LIMIT) {
                /* The holder is not coming back.  A hung board that says which
                 * lock and which core is worth a hundred blind reboots; a panic
                 * here cannot make things worse. */
                raw_puts("\r\n[spin] cpu ");
                raw_putdec((uint32_t)plat_cpu_id());
                raw_puts(" stuck on lock ");
                raw_puthex((uint32_t)(uintptr_t)v);
                raw_puts(" ticks=");
                raw_putdec((uint32_t)plat_ticks());
                raw_puts("\r\n");
                plat_panic_halt();
            }
        }
#if defined(TOS_ARM_SIO_SPIN)
        {
            uint32_t pm = sio_gate_irq_save();

            while (!sio_gate_claim()) { }
            __asm__ volatile("dmb" ::: "memory");   /* order the gate vs *v */
            if (*v == 0) {
                *v = 1;
                __asm__ volatile("dmb" ::: "memory");
                sio_gate_free();
                sio_gate_irq_restore(pm);
                __asm__ volatile("dmb" ::: "memory");
                return;
            }
            sio_gate_free();
            sio_gate_irq_restore(pm);
            /* Lost the race: go back and watch the flag. */
        }
#else
        if (arm_ldrex(v) == 0 && arm_strex(1, v) == 0) {
            __asm__ volatile("dmb" ::: "memory");   /* acquire: later reads see the holder's writes */
            return;
        }
        /* Do not leave the exclusive tag set across the retry spin. */
        __asm__ volatile("clrex" ::: "memory");
#endif
    }
}

int plat_spin_try_acquire(volatile int *v)
{
#if defined(TOS_ARM_SIO_SPIN)
    uint32_t pm = sio_gate_irq_save();

    while (!sio_gate_claim()) { }
    __asm__ volatile("dmb" ::: "memory");
    if (*v == 0) {
        *v = 1;
        __asm__ volatile("dmb" ::: "memory");
        sio_gate_free();
        sio_gate_irq_restore(pm);
        return 1;
    }
    sio_gate_free();
    sio_gate_irq_restore(pm);
    return 0;
#else
    if (arm_ldrex(v) != 0) {
        /* Do not leave the exclusive tag set: a later LDREX on this core
         * would otherwise be the one that clears it, and in between a STREX
         * elsewhere behaves surprisingly. */
        __asm__ volatile("clrex" ::: "memory");
        return 0;
    }
    if (arm_strex(1, v) != 0)
        return 0;
    __asm__ volatile("dmb" ::: "memory");
    return 1;
#endif
}

void plat_spin_release(volatile int *v)
{
    __asm__ volatile("dmb" ::: "memory");   /* release: our writes land before the unlock */
    *v = 0;
}

#endif /* SMP-capable ARM */

/* ---- user mode ------------------------------------------------------- */

#if !defined(TOS_ARM_M0P) && !defined(TOS_ARM_NO_MPU)

/* Two MPU dialects share this file.  The Cortex-M3 (qemu-mps2) has the
 * ARMv7-M one: RASR carries a size field, and the attribute word says both
 * "privileged/user" and cache policy.  The Cortex-M33 (RP2350) has PMSAv8:
 * RBAR/RLAR carry a base and an inclusive limit, permissions live in the
 * base register, and the memory type is an index into MAIR.  The task
 * protection model on top - region numbering, priority, the 32-byte
 * minimum, w^X - is the same, so only the two writers and the three
 * attribute words differ. */

#if defined(TOS_ARM_MPU_V8)

/* PMSAv8.  Region n: RBAR = base | AP | XN, RLAR = limit | AttrIndx << 1 |
 * enable.  base and limit are 32-byte granular, and a region still has to
 * be a power of two with base aligned to it - the limit form does not buy
 * arbitrary boundaries.
 *
 * AttrIndx 0 is SRAM: normal, non-cacheable.  The RP2350's M33 has no data
 * cache, and the LCD's DMA reads row buffers straight out of this memory,
 * so "non-cacheable" is the honest description rather than a pessimistic
 * one. */
/* PMSAv8 packs AP into two bits (RBAR bits 2:1), and the four values are
 * 00 privileged RW, 01 RW at both levels, 10 privileged read-only,
 * 11 read-only at both levels.  Using 10 for "read-only" - the ARMv7-M
 * half-word that means "read-only, both worlds" - left user mode with no
 * access at all and the first instruction fetch faulted (measured: MemManage
 * cfsr=0x1 at win_base). */
#define MPU_ATTR_WINDOW  (1u << 1) | 1u   /* AP 01 RW both, XN              */
#define MPU_ATTR_CODE    (3u << 1)        /* AP 11 RO both, executable      */
#define MPU_ATTR_GUARD   (0u << 1) | 1u   /* AP 00 priv RW only, XN         */

static void mpu_region(uint32_t n, uint32_t base, uint32_t size, uint32_t attr)
{
    uint32_t limit = base + size - ARM_MPU_MIN_SIZE;

    mmio_write32(ARM_MPU_RNR, n);
    mmio_write32(ARM_MPU_RBAR, base | (attr & 0xFu));
    mmio_write32(ARM_MPU_RLAR, limit | 1u);      /* AttrIndx 0, enabled */
}

static void mpu_disable(uint32_t n)
{
    mmio_write32(ARM_MPU_RNR, n);
    mmio_write32(ARM_MPU_RLAR, 0);
}

/* PMSAv8 describes any area whose size is a multiple of 32 bytes with a
 * single region - the base/limit form is what makes that possible, and it is
 * the whole reason this port does not need the block-splitting the ARMv7-M
 * one does.  Two areas (code, data) plus the guard fit in three regions. */
static uint32_t mpu_area(uint32_t n, uint32_t base, uint32_t end, uint32_t attr)
{
    if (end <= base || n >= 7)
        return n;
    mpu_region(n, base, end - base, attr);
    return n + 1;
}

#else  /* ARMv7-M: Cortex-M3 and friends */

#define MPU_ATTR_WINDOW  (ARM_MPU_AP_FULL | ARM_MPU_SRAM | ARM_MPU_XN)
#define MPU_ATTR_CODE    (ARM_MPU_AP_RO | ARM_MPU_SRAM)
#define MPU_ATTR_GUARD   (ARM_MPU_AP_PRIV_RW | ARM_MPU_SRAM | ARM_MPU_XN)
/* The degraded window: RW *and* executable.  Only used when the region
 * budget cannot describe the precise shape - see plat_task_protect. */
#define MPU_ATTR_WINDOW_RWX (ARM_MPU_AP_FULL | ARM_MPU_SRAM)

/* Program one MPU region.  `size` must be a power of two, at least 32 bytes
 * (the ARMv7-M minimum), and `base` aligned to it; the RASR size field
 * stores log2(size)-1, so 4 KiB is 11 and 256 KiB is 17. */
static void mpu_region(uint32_t n, uint32_t base, uint32_t size, uint32_t attr)
{
    uint32_t log2 = 0;

    while ((1u << log2) < size)
        log2++;
    mmio_write32(ARM_MPU_RNR, n);
    mmio_write32(ARM_MPU_RBAR, base);
    mmio_write32(ARM_MPU_RASR, attr | ((log2 - 1u) << 1) | 1u);
}

static void mpu_disable(uint32_t n)
{
    mmio_write32(ARM_MPU_RNR, n);
    mmio_write32(ARM_MPU_RASR, 0);
}

/* ARMv7-M has no base/limit form: an area that is not a naturally aligned
 * power of two becomes several blocks, smallest first. */
static uint32_t mpu_area(uint32_t n, uint32_t base, uint32_t end, uint32_t attr)
{
    while (base < end && n < 7) {
        uint32_t size = ARM_MPU_MIN_SIZE;

        while ((base & (2u * size - 1u)) == 0 && base + 2u * size <= end)
            size *= 2;
        mpu_region(n++, base, size, attr);
        base += size;
    }
    return n;
}

/* How many blocks mpu_area() would spend on [base, end), without touching
 * the hardware.  The region budget is the whole reason this exists: an
 * area whose start is not nicely aligned costs one block per bit, and a
 * task that silently runs out of regions loses its stack region and dies
 * on the first push with no hint as to why. */
static uint32_t mpu_blocks(uint32_t base, uint32_t end)
{
    uint32_t n = 0;

    while (base < end) {
        uint32_t size = ARM_MPU_MIN_SIZE;

        while ((base & (2u * size - 1u)) == 0 && base + 2u * size <= end)
            size *= 2;
        base += size;
        n++;
    }
    return n;
}

#endif

/* Per-task MPU.  Two dialects, two shapes.
 *
 * PMSAv8 (M33) describes an area with a base and an inclusive limit, 32
 * bytes of granularity, no alignment or power-of-two requirement, so the
 * four areas need four regions and nothing else:
 *
 *   0  RO, X    [win_base, wx)        the image
 *   1  RW, XN   [wx, brk)             data and the heap that sbrk() has
 *                                     handed out
 *   2  RW, XN   [stack_low, win_end)  the stack
 *   7  --       the hole under the stack   stack overflow guard
 *
 * Nothing claims the gap between the break and the stack, so a heap that
 * runs off its end faults exactly like a stack that runs off its bottom -
 * the same shape rv32 gets from PMP (code RX, heap and stack RW with X off,
 * the hole in between denied).
 *
 * ARMv7-M (M3) has no such form: a region is a naturally aligned power of
 * two and RASR stores its log2.  Covering an arbitrary [a,b) there costs
 * one region per block, which is why that dialect keeps the coarser shape -
 * the code area in doubling blocks, then everything above it writable:
 *
 *   0..k   RO       [win_base, wx) in aligned power-of-two blocks
 *   k..6   RW, XN   the rest of the window, in aligned power-of-two blocks
 *   7      --       the hole under the stack   stack overflow guard
 *
 * Region numbers are priority: a higher number wins where two overlap.  The
 * v8 shape never relies on that - its areas do not overlap at all, which is
 * deliberate: an earlier version overlapped the read-only blocks over the
 * writable ones and got an instruction-access fault on the very first
 * fetch.  The v7 shape does rely on it, so the read-only blocks sit on top.
 * Because the writable blocks are execute-never, only the read-only ones
 * can be executed: that is w^X.
 *
 * Everything outside the window - kernel RAM, MMIO, other tasks - matches
 * no region and is denied to user mode (the kernel still reaches it through
 * PRIVDEFENA).
 *
 * The window needs more than one region on ARMv7-M whenever its size is not
 * a power of two.  That is not hypothetical: windows are whole grains, and
 * a 48 KiB window (a 24 KiB image plus stack and guard, on the RP2350's
 * 16 KiB grain) is a real case.  Describing it with the largest power of
 * two that fits would leave the top of the window - where the stack lives -
 * uncovered, and the first push would fault.
 *
 * The one boundary even PMSAv8 cannot express exactly is `brk`, because 32
 * bytes is the granularity: the heap region ends at brk rounded *up*, so
 * the last partial block stays usable and no legitimate allocation fails
 * (rounding down would fault on a chunk that straddles the boundary).  A
 * write lands in that slack only within 31 bytes of the break; past it the
 * fault is real.  user/prot.c accounts for the slack when it probes.
 *
 * mpu_region/mpu_disable are not atomic with respect to the tick.  That is
 * fine: a handler taken mid-update runs privileged and access to RAM and
 * MMIO does not depend on these regions, and process_tick only reschedules
 * on a user frame. */
/* Set by the first call on each core: see the note in plat_init about why
 * the MPU is not switched on during boot.  Per core because the MPU, its
 * MAIR and MPU_CTRL are per-core registers: core 1 that skipped this would
 * run its tasks with no regions at all, i.e. with the whole flat address
 * space reachable from user mode. */
static int mpu_on[MAX_CPUS];

void plat_task_protect(uint32_t win_base, uint32_t win_end, uint32_t wx,
                       uint32_t brk, uint32_t stack_low, uint32_t stack_top)
{
    uint32_t r;
    int cid = plat_cpu_id();
    static int reported;

    if (cid < 0 || cid >= (int)MAX_CPUS)
        cid = 0;

    (void)stack_top;

    /* wx is 32-byte aligned by user.ld; round up defensively so a stale
       script cannot turn the tail of .text into non-executable memory. */
    wx = (wx + ARM_MPU_MIN_SIZE - 1u) & ~(ARM_MPU_MIN_SIZE - 1u);
    if (wx > win_end)
        wx = win_end;

#if defined(TOS_ARM_MPU_V8)
    /* The heap ends where sbrk() has taken it, rounded up to the region
       granularity.  Clamped to the stack so a stale brk cannot make the two
       areas overlap - overlapping areas are the one thing this shape must
       not have. */
    brk = (brk + ARM_MPU_MIN_SIZE - 1u) & ~(ARM_MPU_MIN_SIZE - 1u);
    if (brk > stack_low)
        brk = stack_low;

    r = 0;
    r = mpu_area(r, win_base, wx, MPU_ATTR_CODE);
    r = mpu_area(r, wx, brk, MPU_ATTR_WINDOW);         /* data + heap */
    r = mpu_area(r, stack_low, win_end, MPU_ATTR_WINDOW);  /* stack */
#else
    (void)brk;

    /* The precise shape needs one block per zero bit-run at the area's
     * start, at both ends of the split - a 21 KiB image at wx-win_base =
     * 0x5420 costs 4 code blocks and 9 data blocks, against a budget of 7.
     * When it does not fit, a precise shape truncated at the budget would
     * leave the window's upper half (the stack) uncovered and the first
     * push would fault with no explanation.  So measure first and fall back
     * to one whole-window RWX area, which still bounds the task to its own
     * window and keeps the stack guard: less protection, but a task that
     * runs and a message that says why. */
    if (mpu_blocks(win_base, wx) + mpu_blocks(wx, win_end) <= 7) {
        r = 0;
        r = mpu_area(r, win_base, wx, MPU_ATTR_CODE);
        r = mpu_area(r, wx, win_end, MPU_ATTR_WINDOW);
    } else {
        static int warned;

        r = 0;
        r = mpu_area(r, win_base, win_end, MPU_ATTR_WINDOW_RWX);
        if (!warned) {
            warned = 1;
            kprintf("[mpu] window needs %u+%u blocks (budget 7): code is "
                    "writable and executable for this task\n",
                    (unsigned)mpu_blocks(win_base, wx),
                    (unsigned)mpu_blocks(wx, win_end));
        }
    }
#endif
    /* How many regions the areas above actually used.  Counted before the
       disable loop: after it every index is 7, and reporting that (as this
       line used to) says nothing about the shape that was programmed. */
    uint32_t used = r;
    while (r < 7)
        mpu_disable(r++);

    /* A stack that grows past its bottom hits this block: user mode has no
       access, the kernel still does.  Its size is the hole process.c keeps
       between the break and the stack, so no legitimate heap write lands
       here. */
    mpu_region(7, stack_low - PLAT_HEAP_GUARD, PLAT_HEAP_GUARD,
               MPU_ATTR_GUARD);

    if (!mpu_on[cid]) {
        mpu_on[cid] = 1;
#if defined(TOS_ARM_MPU_V8)
        /* Attribute byte 0 (the only one the task regions index) is normal,
           non-cacheable memory: the M33 in the RP2350 has no data cache and
           the panel's DMA reads row buffers straight out of SRAM, so there
           is nothing to keep coherent.  Written here rather than at boot so
           that the ROM's own regions never see a changed attribute. */
        mmio_write32(ARM_MPU_MAIR0, 0x44444444u);
        mmio_write32(ARM_MPU_MAIR1, 0x00000000u);
#endif
        mmio_write32(ARM_MPU_CTRL,
                     ARM_MPU_CTRL_ENABLE | ARM_MPU_CTRL_PRIVDEFENA);
    }

    if (!reported) {
        reported = 1;
        kprintf("[mpu] type=%x win=%x..%x wx=%x brk=%x stk=%x..%x regions=%u\n",
                (unsigned)mmio_read32(ARM_MPU_TYPE),
                (unsigned)win_base, (unsigned)win_end, (unsigned)wx,
                (unsigned)brk,
                (unsigned)(stack_low - PLAT_HEAP_GUARD), (unsigned)stack_low,
                (unsigned)used);
    }

    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

#else /* M0+ or TOS_ARM_NO_MPU: user tasks run unprivileged but unprotected. */

void plat_task_protect(uint32_t win_base, uint32_t win_end, uint32_t wx,
                       uint32_t brk, uint32_t stack_low, uint32_t stack_top)
{
    (void)win_base; (void)win_end; (void)wx; (void)brk;
    (void)stack_low; (void)stack_top;
}

#endif

void plat_prepare_user_frame(regs_t *frame, uint32_t entry, uint32_t sp)
{
    memset(frame, 0, sizeof(*frame));

    /* The hardware frame (32 bytes) sits *below* whatever process.c packed
       on the user stack (argc/argv), and sp points just under it.  The core
       pops those 32 bytes on the exception return that starts the task, so
       the first instruction sees sp pointing at [argc][argv..][NULL]. */
    uint32_t *hw = (uint32_t *)((sp - ARM_HW_SIZE) & ~7u);
    memset(hw, 0, ARM_HW_SIZE);
    hw[5] = 0;                             /* lr: returning from main exits */
    hw[6] = entry | 1u;                    /* pc: Thumb bit must be set     */
    hw[7] = ARM_XPSR_THUMB;                /* xpsr                          */

    frame->hw = (uint32_t)hw;
    frame->pc = hw[6];
    frame->xpsr = hw[7];
    frame->lr = 0;
    frame->exc_return = ARM_EXC_RET_THREAD;

    /* arm_enter_user restores MSP from this.  A task that has never run has
       no kernel frame of its own, so start from the top of the kernel stack
       of whichever core prepares it: the next trap overwrites whatever is
       below it anyway, and a task may start on either core. */
    frame->kern_sp = CPU_KSTACK_TOP((uint32_t)plat_cpu_id());
}

/* ARM syscall convention: r0 = number, r1/r2/r3 = arguments, r0 = result.
 * These live in the hardware-frame copy. */
void plat_frame_get_syscall(const regs_t *r, syscall_t *sc)
{
    sc->nr = r->r0;
    sc->a0 = r->r1;
    sc->a1 = r->r2;
    sc->a2 = r->r3;
}

void plat_frame_set_ret(regs_t *r, uint32_t val) { r->r0 = val; }
uint32_t plat_frame_get_ret(const regs_t *r)    { return r->r0; }

int plat_fault_is_user(void *frame)
{
    return frame_is_user((const regs_t *)frame);
}

/* Implemented in start.S. */
extern void arm_enter_user(void *frame) __attribute__((noreturn));

void plat_enter_user(void *frame)
{ arm_enter_user(frame);
}

void plat_set_kernel_stack(uint32_t top)
{
    /* MSP is per core, and a task that migrates to the other core carries a
     * frame whose kern_sp was recorded on the core it last ran on.  Restoring
     * that value would point MSP into the other core's stack, and the next
     * trap there would push its frame on top of whatever that core was
     * doing - measured on the RP2350 as the shell dying with a corrupted
     * buffer pointer a second after boot, because the two cores had been
     * stacking exceptions in the same 16 KiB.
     *
     * So: whenever the scheduler installs a task on this CPU, rewrite the
     * frame's kernel stack to this CPU's.  The port hook exists for exactly
     * this; the plain ARM behaviour of "MSP comes from the vector table and
     * never changes" is only true on one core. */
    regs_t *r = process_current_frame();

    if (r)
        r->kern_sp = top;
}

/* Optional board hook, run inside the panic loop.  A pointer instead of a
 * weak symbol because the C compilers used here do not all implement
 * __attribute__((weak)), and a board that does not set it keeps the old
 * behaviour exactly.  The rp2350 port points this at a Ctrl-D poll so a
 * dead kernel can still be pushed back into the ROM bootloader without
 * physical access to the BOOTSEL button. */
void (*g_panic_poll)(void);

/* USBCTRL (IRQ 14) body.  The vector table is shared by every ARM board, so
 * the stub in start.S cannot name a board symbol; the port that owns the
 * controller installs its handler here.  Boards without USB leave it NULL
 * and the vector just returns. */
void (*g_usb_irq_handler)(void);

void arm_usbctrl_irq_c(void)
{
    if (g_usb_irq_handler)
        g_usb_irq_handler();
}

void plat_panic_halt(void)
{
    for (;;) {
        if (g_panic_poll) {
            g_panic_poll();
            /* Spin instead of WFI: a panic can happen with interrupts
             * masked (early init, or a fault inside a critical section),
             * and WFI would then never wake to poll the console again. */
            for (volatile int i = 0; i < 200; i++)
                ;
        } else {
            __asm__ volatile("wfi");
        }
    }
}


