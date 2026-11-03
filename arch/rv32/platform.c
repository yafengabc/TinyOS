#include "platform.h"
#include "board.h"
#include "regs.h"
#include "console.h"
#include "string.h"
#include "process.h"
#include "tty.h"

/* Generic RV32 platform layer, shared by every RV32 board.
 *
 * What lives here is identical across boards: machine-mode trap dispatch,
 * U-mode via PMP, interrupt enable/disable, the panic halt.  What differs
 * - console, timer, block device, shutdown - belongs to the board and is
 * reached through the hooks in board.h.  There is no MMU on this
 * architecture: all addresses are physical and task windows come from the
 * same bump allocator the x86 port uses (kernel/process.c). */

/* ---- traps ----------------------------------------------------------- */

static int from_user(const regs_t *f)
{
    return (f->mstatus & RV_MSTATUS_MPP_MASK) == RV_MSTATUS_MPP_U;
}

static const char *cause_name(uint32_t cause)
{
    switch (cause) {
    case 0:  return "instruction address misaligned";
    case 1:  return "instruction access fault";
    case RV_MCAUSE_ILLEGAL:      return "illegal instruction";
    case 3:  return "breakpoint";
    case 4:  return "load address misaligned";
    case RV_MCAUSE_LOAD_FAULT:   return "load access fault";
    case 6:  return "store address misaligned";
    case RV_MCAUSE_STORE_FAULT:  return "store access fault";
    case RV_MCAUSE_ECALL_U:      return "ecall from U-mode";
    case 11: return "ecall from M-mode";
    default: return "unknown trap";
    }
}

regs_t *trap_handler(regs_t *f)
{
    uint32_t cause = f->mcause;

    /* qemu-virt routes the tick through the CLINT (cause 7); the C3 has no
     * CLINT and its INTMTX reports the routed CPU LINE directly:
     * mcause = 0x80000000 | line, so the tick on line 2 arrives as
     * 0x80000002 - NOT machine-external cause 11 (verified on the real
     * chip).  Both boards have exactly one interrupt source, so any
     * interrupt-bit cause is the tick; which one fired is a board
     * property, so the rearm/count lives behind plat_timer_tick(). */
    if (cause & RV_MCAUSE_IRQ_BIT) {
        uint32_t line = cause & ~RV_MCAUSE_IRQ_BIT;

        if (!plat_irq_is_tick(line)) {
            /* A board interrupt that is not the clock - on the C3 that is
             * console input.  Drain it and go straight back to whatever we
             * interrupted: running the tick path here would spend a time
             * slice and advance the clock once per received byte.  Drain
             * first, acknowledge second: the timeout bit latches, and
             * acking before the FIFO is empty lets it set again. */
            tty_poll();
            plat_irq_service(line);
            return f;
        }

        plat_timer_tick();
        process_tick(f);
        return process_resched(f);
    }

    if (cause == RV_MCAUSE_ECALL_U) {
        /* Step over the ecall first, so the frame reads like the i386 one
         * (where the CPU has already pushed the return address): exec and
         * exit save/replace this frame, and the saved copy must resume
         * after the syscall rather than re-execute it.  ecall is always a
         * 4-byte instruction. */
        f->mepc += 4;
        return syscall_dispatch(f);
    }

    if (from_user(f)) {
        /* A user program trapped: kill it and keep the system alive. */
        kprintf("\n[pid %d] %s at 0x%08x", process_current_pid(),
                cause_name(cause), f->mepc);
        if (cause == RV_MCAUSE_LOAD_FAULT || cause == RV_MCAUSE_STORE_FAULT)
            kprintf(" (address 0x%08x)", f->mtval);
        kprintf("\n");
        return process_exit(f, 139);
    }

    kprintf("\n*** RV32 TRAP ***\n");
    kprintf("%s: mcause=0x%x mepc=0x%x mtval=0x%x\n",
            cause_name(cause), cause, f->mepc, f->mtval);
    plat_panic_halt();
    return f;
}

/* ---- boot ------------------------------------------------------------ */
/* U-mode reaches memory only through PMP.  Entries 4..15 stay off, so
 * until the first task is programmed the user side can touch nothing at
 * all - M-mode is not subject to PMP as long as no entry sets the lock
 * bit, which is what keeps the flat kernel model working.
 *
 * All four config registers exist on qemu-virt and on the ESP32-C3 (16
 * entries, none preset - verified on the real chip, see
 * scripts/esp-c3/probe.sh pmp).  A core with fewer would need the count
 * probed first, and a bootloader that locked its entries cannot be
 * overruled at all. */
static void pmp_deny_user(void)
{
    __asm__ volatile("csrw pmpcfg0, zero");
    __asm__ volatile("csrw pmpcfg1, zero");
    __asm__ volatile("csrw pmpcfg2, zero");
    __asm__ volatile("csrw pmpcfg3, zero");
}

void plat_early_init(void)
{
    plat_board_early();
}

int plat_cpu_id(void)
{
    uint32_t id;

    /* Every RISC-V implementation in machine mode has mhartid (zero on the
     * single-hart ESP32-C3, 0..n-1 on qemu-virt). */
    __asm__ volatile("csrr %0, mhartid" : "=r"(id));
    return (int)id;
}

extern char trap_vectors[];

static void pmp_open_trap_vector(void);

void plat_init(void)
{
    pmp_deny_user();
    plat_board_init();
    pmp_open_trap_vector();
}

/* PMP access bits, shared by the boot-time vector opening and the
 * per-task programming in plat_task_protect. */
#define PMP_R     0x01u
#define PMP_W     0x02u
#define PMP_X     0x04u
#define PMP_TOR   0x08u
#define PMP_NAPOT 0x18u

/* The trap entry must be reachable from U-mode.  The RISC-V spec has the
 * first instruction fetch of a trap run at the NEW privilege level (so
 * M-mode fetches ignore PMP and any mtvec target works), and qemu-virt
 * behaves that way.  The esp-riscv core in the ESP32-C3 does not: a tick
 * interrupt taken while a user program was running faulted with
 * mcause=1 (instruction access fault) at mtvec+4*2, i.e. the vector slot
 * itself was PMP-checked with the OLD (U-mode) rights (ls on the real
 * chip died at 0x40382c08 = trap_vectors slot 2).  Give U-mode execute
 * permission on the 256-byte vector table via spare entry 4; the handler
 * bodies those jumps reach stay inside the PMP-denied kernel region, and
 * by the time they run the core is in M-mode anyway.  plat_task_protect
 * only touches entries 0..3 and pmpcfg0, so this entry survives every
 * task switch. */
extern char trap_vectors[];

static void pmp_open_trap_vector(void)
{
    uint32_t base = (uint32_t)(uintptr_t)trap_vectors;
    uint32_t addr = (base >> 2) | ((256u >> 3) - 1u);   /* NAPOT, 256 B */
    uint32_t cfg = PMP_NAPOT | PMP_R | PMP_X;           /* entry 4 bits 7:0 */

    __asm__ volatile("csrw pmpaddr4, %0" : : "r"(addr));
    __asm__ volatile("csrw pmpcfg1, %0" : : "r"(cfg));
}

/* ---- interrupts ------------------------------------------------------ */

void plat_irq_enable(void)
{
    __asm__ volatile("csrsi mstatus, 8");   /* mstatus.MIE = bit 3 */
}

void plat_irq_disable(void)
{
    __asm__ volatile("csrci mstatus, 8");
}

void plat_cpu_idle(void)
{
    __asm__ volatile("csrsi mstatus, 8; wfi");
}

/* ---- user mode ------------------------------------------------------- */

/* Per-task PMP.  TOR is positional - entry i covers [pmpaddr[i-1]<<2,
 * pmpaddr[i]<<2) - and the lower bound comes from the previous address
 * register even when that entry is disabled.  Since entry 0 can only
 * start at address 0, that is how a region below the window is denied:
 * entry 0 is off and holds nothing but the lower bound.
 *
 *   0  off    win_base                     lower bound only
 *   1  RX     [win_base, wx)               code
 *   2  RW     [wx, brk)                    data and heap
 *   3  RW     [stack_low, stack_top)       stack, NAPOT
 *   (4  RX     trap_vectors, 256 B          boot time, see below)
 *
 * Nothing claims the hole between brk and stack_low, so a heap that runs
 * off its end and a stack that runs off its bottom both fault instead of
 * quietly eating the other.  The stack uses NAPOT because it is absolute:
 * a TOR entry there would have to start at brk and would fill that hole.
 * Entry 4 (vector table, see pmp_open_trap_vector) is boot-time only. */
#define PMP_R     0x01u
#define PMP_W     0x02u
#define PMP_X     0x04u
#define PMP_TOR   0x08u
#define PMP_NAPOT 0x18u

void plat_task_protect(uint32_t win_base, uint32_t wx, uint32_t brk,
                       uint32_t stack_low, uint32_t stack_top)
{
    uint32_t size = stack_top - stack_low;

    __asm__ volatile("csrw pmpaddr0, %0" : : "r"(win_base >> 2));
    __asm__ volatile("csrw pmpaddr1, %0" : : "r"((wx  & ~3u) >> 2));
    __asm__ volatile("csrw pmpaddr2, %0" : : "r"((brk & ~3u) >> 2));
    /* NAPOT: all ones above the size bit, so 32 KiB is (32K>>3)-1 = 0xFFF. */
    __asm__ volatile("csrw pmpaddr3, %0" : :
                     "r"((stack_low >> 2) | ((size >> 3) - 1u)));

    uint32_t cfg = ((PMP_TOR   | PMP_R | PMP_X) << 8)    /* entry 1 */
                 | ((PMP_TOR   | PMP_R | PMP_W) << 16)   /* entry 2 */
                 | ((PMP_NAPOT | PMP_R | PMP_W) << 24);  /* entry 3 */
    __asm__ volatile("csrw pmpcfg0, %0" : : "r"(cfg));
}

void plat_prepare_user_frame(regs_t *frame, uint32_t entry, uint32_t sp)
{
    memset(frame, 0, sizeof(*frame));
    frame->sp = sp;
    frame->mepc = entry;
    /* Return to U-mode with interrupts "enabled" (MPIE), so mret leaves
       the machine in the same state a fresh trap from user code finds. */
    frame->mstatus = RV_MSTATUS_MPP_U | RV_MSTATUS_MPIE;
}

/* RV32 syscall convention: a7 = number, a0/a1/a2 = arguments, a0 = result. */
void plat_frame_get_syscall(const regs_t *r, syscall_t *sc)
{
    sc->nr = r->a7;
    sc->a0 = r->a0;
    sc->a1 = r->a1;
    sc->a2 = r->a2;
}

void plat_frame_set_ret(regs_t *r, uint32_t val) { r->a0 = val; }
uint32_t plat_frame_get_ret(const regs_t *r)    { return r->a0; }

int plat_fault_is_user(void *frame)
{
    return from_user((const regs_t *)frame);
}

/* Implemented in start.S: restores the frame and mret's into it. */
extern void rv32_enter_user(void *frame) __attribute__((noreturn));

void plat_enter_user(void *frame)
{
    rv32_enter_user(frame);
}

void plat_set_kernel_stack(uint32_t top)
{
    /* The kernel stack lives in .bss and start.S computes it directly;
       there is no TSS to update. */
    (void)top;
}

void plat_panic_halt(void)
{
    __asm__ volatile("csrci mstatus, 8");
    for (;;)
        __asm__ volatile("wfi");
}

/* ---- spin locks -------------------------------------------------------
 *
 * RISC-V gives this for free in the A extension: amoswap.w with the .aqrl
 * ordering bits is a test-and-set with both acquire and release semantics,
 * so one instruction is the whole primitive.  A plain swap is enough for
 * the lock/unlock pair because the ordering bits - not the swap - are what
 * make the critical section visible in the right order to the other hart.
 *
 * A build without the A extension (rv32imc: every board here except the SMP
 * target) cannot assemble this at all, and does not need to: spin_lock() in
 * kernel/cpu.c never calls it when cpu_count is 1, and kernel/cpu.c carries
 * weak no-op defaults for exactly that case.  Guarding it at compile time is
 * what keeps the single-hart ports building, rather than relying on a
 * link-time trick. */
#if TOS_MAX_CPUS > 1
void plat_spin_acquire(volatile int *v)
{
    uint32_t got;

    do {
        __asm__ volatile("amoswap.w.aqrl %0, %1, (%2)"
                         : "=r"(got)
                         : "r"(1), "r"(v)
                         : "memory");
    } while (got != 0);
}

void plat_spin_release(volatile int *v)
{
    /* A release store is what orders the critical section's writes before
       the unlock: the other hart sees the data before it sees the lock. */
    __asm__ volatile("fence rw, rw" ::: "memory");
    *v = 0;
}
#endif
