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

/* DIAG: a kernel-mode trap during early init would print through kprintf,
 * whose console ring does not exist yet - the output would vanish and a
 * second fault inside kprintf would spin forever.  This writes straight to
 * the board console so such a fault is visible. */
static void diag_hex(uint32_t v)
{
    static const char hx[] = "0123456789abcdef";

    plat_console_putc('0');
    plat_console_putc('x');
    for (int i = 28; i >= 0; i -= 4)
        plat_console_putc(hx[(v >> i) & 0xfu]);
}

static void diag_kernel_trap(uint32_t cause, uint32_t mepc, uint32_t mtval)
{
    plat_console_putc('\r');
    plat_console_putc('\n');
    plat_console_putc('!');
    plat_console_putc('t');
    plat_console_putc('r');
    plat_console_putc('a');
    plat_console_putc('p');
    plat_console_putc(' ');
    diag_hex(cause);
    plat_console_putc(' ');
    diag_hex(mepc);
    plat_console_putc(' ');
    diag_hex(mtval);
    plat_console_putc('\r');
    plat_console_putc('\n');
}

regs_t *trap_handler(regs_t *f)
{
    uint32_t cause = f->mcause;

    /* A ktask's frame arrived on its own stack (start.S checked the flag);
     * mret must go back to M-mode even though the hardware cleared MPP when
     * the thread was first entered.  Force the saved copy before any of the
     * from_user()/reschedule decisions look at it. */
    if (ktask_running)
        f->mstatus = (f->mstatus & ~RV_MSTATUS_MPP_MASK) | RV_MSTATUS_MPP_M;

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
            /* Give a board's non-console source (the C3 WiFi MAC) first
             * refusal.  Weak so boards without one link to zero. */
            extern int plat_irq_foreign(uint32_t line) __attribute__((weak));

            if (plat_irq_foreign && plat_irq_foreign(line))
                return process_resched(f);

            /* A board interrupt that is not the clock - on the C3 that is
             * console input (UART0 on one line, the USB-Serial/JTAG packet
             * source on another).  Drain it and go straight back to
             * whatever we interrupted: running the tick path here would
             * spend a time slice and advance the clock once per received
             * byte.  Drain first, acknowledge second: the timeout bit
             * latches, and acking before the FIFO is empty lets it set
             * again.  Then hand the fresh bytes to whoever is blocked
             * reading them - waiting for the next tick would cap every
             * console transfer at the tick rate.  Drain the echo queue here
             * too: the RX interrupt bypasses the tick, and without this the
             * freshly cooked keystrokes sit in the ring until the next 1 ms
             * tick while the woken shell is already printing its prompt. */
            tty_poll();
            plat_irq_service(line);
            console_echo_tick();
            process_console_kick();

            /* The kick can have made a more important task runnable, and
             * "a wake-up preempts" has to mean here too, not "within one
             * tick": returning the frame unconditionally made an early
             * wake-up wait for the timer.  process_resched() is the same
             * call the tick path ends with, and it is a branch when
             * nothing is pending. */
            return process_resched(f);
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

    if (cause == RV_MCAUSE_ECALL_M) {
        f->mepc += 4;
        return process_ktask_syscall(f);
    }

    if (from_user(f)) {
        /* A load or store that ran off the bottom of the stack may be the
         * stack running out rather than a bug: the guard hole under it is
         * exactly where the first byte past the end lands.  If the task's
         * window still has heap slack, the stack is doubled into it and the
         * instruction is retried - it faulted before writing, so there is
         * nothing to undo.  Everything else (and a growth that the port
         * cannot encode, or that the heap is in the way of) dies as before. */
        if ((cause == RV_MCAUSE_LOAD_FAULT || cause == RV_MCAUSE_STORE_FAULT) &&
            process_stack_grow(f->mtval, f->sp))
            return f;

        /* A user program trapped: kill it and keep the system alive. */
        kprintf("\n[pid %d] %s at 0x%08x", process_current_pid(),
                cause_name(cause), f->mepc);
        if (cause == RV_MCAUSE_LOAD_FAULT || cause == RV_MCAUSE_STORE_FAULT)
            kprintf(" (address 0x%08x)", f->mtval);
        kprintf("\n");
        return process_exit(f, 139);
    }

    diag_kernel_trap(cause, f->mepc, f->mtval);
    kprintf("\n*** RV32 TRAP ***\n");
    kprintf("%s: mcause=0x%x mepc=0x%x mtval=0x%x\n",
            cause_name(cause), cause, f->mepc, f->mtval);
    kprintf("ra=0x%08x sp=0x%08x s0=0x%08x s1=0x%08x a0=0x%08x a1=0x%08x\n",
            f->ra, f->sp, f->s0, f->s1, f->a0, f->a1);
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
    plat_console_putc('a'); /* DIAG */
    plat_board_init();
    plat_console_putc('b'); /* DIAG */
    pmp_open_trap_vector();
    plat_console_putc('c'); /* DIAG */
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

/* csrrci reads the old mstatus and clears MIE in one instruction, so the
 * saved value cannot be torn by an interrupt in between. */
uint32_t plat_irq_save(void)
{
    uint32_t old;

    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(old) : : "memory");
    return old & 8u;
}

void plat_irq_restore(uint32_t state)
{
    if (state)
        __asm__ volatile("csrsi mstatus, 8" ::: "memory");
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

void plat_task_protect(uint32_t win_base, uint32_t win_end, uint32_t wx,
                       uint32_t brk, uint32_t stack_low, uint32_t stack_top)
{
    uint32_t size = stack_top - stack_low;

    /* A board whose PMP cannot express the TOR layout (RP2350 Hazard3 has no
     * TOR matching) programs its own regions. */
    if (plat_board_task_protect(win_base, win_end, wx, brk, stack_low, stack_top))
        return;

    /* The window's end is implied by whichever region is highest (the
       stack), so the TOR/NAPOT entries do not need it.  An ARM-style port
       that describes the whole window would. */
    (void)win_end;

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

/* First argument of a user entry point: thread trampolines take one. */
void plat_frame_set_arg(regs_t *frame, uint32_t arg)
{
    frame->a0 = arg;
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
    /* Spin instead of WFI: a panic can happen with interrupts masked (early
     * init, or a fault inside a critical section), and WFI would then never
     * wake to poll the console again.  The board hook is what keeps a dead
     * RP2350 recoverable from the host. */
    for (;;) {
        plat_board_panic_poll();
        for (volatile int i = 0; i < 200; i++)
            ;
    }
}

__attribute__((weak)) void plat_board_panic_poll(void)
{
}

__attribute__((weak)) int plat_board_task_protect(uint32_t win_base,
                                                  uint32_t win_end,
                                                  uint32_t wx,
                                                  uint32_t brk,
                                                  uint32_t stack_low,
                                                  uint32_t stack_top)
{
    (void)win_base; (void)win_end; (void)wx; (void)brk;
    (void)stack_low; (void)stack_top;
    return 0;
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

/* The ISA string is a property of this image, not of the board: rv32imc on
 * the C3, rv32imac on the RP2350's Hazard3 and the SMP QEMU target. */
const char *plat_cpu_isa(void)
{
#if defined(__riscv_atomic) && defined(__riscv_compressed)
    return "rv32imac";
#elif defined(__riscv_atomic)
    return "rv32ima";
#elif defined(__riscv_compressed)
    return "rv32imc";
#else
    return "rv32im";
#endif
}
