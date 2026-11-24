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
        {
            int user = from_user(f);
            regs_t *r = process_resched(f);

            /* This port's PendSV.  A board with a polled controller (the
             * RP2350's USB CDC) has work that must run in thread context, and
             * the idle loop never runs while some task is runnable - so a
             * program that keeps writing stops the pump and its tail is
             * dropped (OPEN-ITEMS §5c on the ARM ports, whose answer is
             * PendSV).
             * The tick returning to U-mode is the same boundary: the
             * scheduler and every syscall have unwound, no kernel lock is
             * held, and the frame about to be resumed is user code.
             *
             * Interrupts are turned on for the duration because the pump can
             * take longer than one tick: with MIE clear a nested tick would be
             * latched and only serviced afterwards, and plat_timer_tick()
             * re-arms the comparator from the time it finally runs, so every
             * pump would steal time from the clock.  A nested trap here is
             * safe: it comes from M-mode, so process_resched() refuses to
             * switch tasks on its frame (only a frame that interrupted user
             * code may), and the USB critical sections mask interrupts
             * themselves, so this hart cannot deadlock against its own tick's
             * usb_cdc_tx_kick(). */
            if (user && plat_cpu_id() == 0 && board_poll_installed()) {
                plat_irq_enable();
                board_poll_deferred();
                plat_irq_disable();
            }
            return r;
        }
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
        /* Hazard3 hardwires mtval to zero (measured on the RP2350: every
         * store fault reports 0), so the faulting address is unavailable.
         * sp is the next best witness and is what the growth decision turns
         * on: if sp left the stack the write is a candidate, and
         * process_stack_grow() refuses it anyway when it did not. */
        uint32_t fault_addr = f->mtval ? f->mtval : f->sp;
        if ((cause == RV_MCAUSE_LOAD_FAULT || cause == RV_MCAUSE_STORE_FAULT) &&
            process_stack_grow(fault_addr, f->sp))
            return f;
        if ((cause == RV_MCAUSE_LOAD_FAULT ||
             cause == RV_MCAUSE_STORE_FAULT) &&
            process_stack_overflow_at(f->sp)) {
            /* The fault was outside the current stack and the growth above
             * either had no room or could not describe the larger range.
             * This is deep 6's shape on RV32: report the same refusal the
             * software-guard ports print instead of a bare store fault. */
            kprintf("\n[pid %d] stack overflow (sp 0x%08x left the stack); "
                    "no room to grow it; killing\n",
                    process_current_pid(), (unsigned)f->sp);
            return process_exit(f, 139);
        }

        /* A user program trapped: kill it and keep the system alive. */
        kprintf("\n[pid %d] %s at 0x%08x", process_current_pid(),
                cause_name(cause), f->mepc);
        if ((cause == RV_MCAUSE_LOAD_FAULT ||
             cause == RV_MCAUSE_STORE_FAULT) && f->mtval)
            kprintf(" (address 0x%08x)", f->mtval);
        kprintf("\n");
        return process_exit(f, 139);
    }

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
    uint32_t cfg1;

    /* Entry 5, not 4: entry 4 is the task's stack region (see
     * plat_task_protect) and is rewritten on every switch.  Address before
     * config, while every A field is still OFF, so the NAPOT value keeps all
     * of the low bits that encode the 256-byte region size. */
    __asm__ volatile("csrw pmpaddr5, %0" : : "r"(addr));
    __asm__ volatile("csrr %0, pmpcfg1" : "=r"(cfg1));
    cfg1 = (cfg1 & ~0xff00u) | ((uint32_t)(PMP_NAPOT | PMP_R | PMP_X) << 8);
    __asm__ volatile("csrw pmpcfg1, %0" : : "r"(cfg1));
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

__attribute__((weak)) void plat_flash_park_poll(void)
{
}

void plat_cpu_idle(void)
{
    /* A core parked in sched_idle() is already inside its timer trap, so the
     * tick check can never see the park flag; poll here before sleeping. */
    plat_flash_park_poll();
    __asm__ volatile("csrsi mstatus, 8; wfi");
}

/* ---- user mode ------------------------------------------------------- */

/* Per-task PMP.  TOR is positional - entry i covers [pmpaddr[i-1]<<2,
 * pmpaddr[i]<<2) - and the lower bound comes from the previous address
 * register even when that entry is disabled.  Since entry 0 can only
 * start at address 0, that is how a region below the window is denied:
 * entry 0 is off and holds nothing but the lower bound.
 *
 * A segment list maps onto this directly, and a gap needs no region of its
 * own - only a boundary to end the piece below it:
 *
 *   0  off    segs[0].base              lower bound only
 *   1  RX     [win_base, wx)            code
 *   2  RW     [wx, brk)                 data and heap
 *   3  off    stack_low                 guard hole, not covered
 *   4  RW     [stack_low, stack_top)    stack
 *   (5  RX     trap_vectors, 256 B      boot time, see below)
 *
 * TOR has no alignment or size rule, so this port follows the segments
 * exactly; an empty segment costs no entry.  Nothing claims the hole between
 * brk and stack_low, so a heap that runs off its end and a stack that runs
 * off its bottom both fault instead of quietly eating the other.
 * Entry 5 (vector table, see pmp_open_trap_vector) is boot-time only. */
#define PMP_R     0x01u
#define PMP_W     0x02u
#define PMP_X     0x04u
#define PMP_TOR   0x08u
#define PMP_NAPOT 0x18u
#define PMP_TOR_ENTRIES 5u          /* 0..4; 5 is the trap vector */

void plat_task_protect(const plat_seg_t *segs, int n)
{
    uint32_t addr[PMP_TOR_ENTRIES];
    uint32_t cfg[PMP_TOR_ENTRIES];
    uint32_t cfg0, cfg1;
    int k = 0;

    /* A board whose PMP cannot express TOR (RP2350's Hazard3) programs its
     * own regions from the same segment list. */
    if (plat_board_task_protect(segs, n))
        return;

    for (unsigned i = 0; i < PMP_TOR_ENTRIES; i++)
        addr[i] = cfg[i] = 0;

    if (n > 0) {
        /* Entry 0 carries only the lower bound: a TOR region needs a start,
         * and anything below it must stay unmatched. */
        addr[0] = segs[0].base >> 2;

        for (int i = 0; i < n; i++) {
            if (i > 0 && segs[i].base > segs[i - 1].top) {
                /* A gap: close the piece below with a disabled entry whose
                 * address is the next piece's base, so that piece starts
                 * there and the hole stays uncovered.  Check the bound
                 * before each increment: a three-segment list with two holes
                 * used to advance k twice past PMP_TOR_ENTRIES. */
                if (k + 1 >= (int)PMP_TOR_ENTRIES)
                    break;
                addr[++k] = segs[i].base >> 2;
            }
            if (k + 1 >= (int)PMP_TOR_ENTRIES)
                break;
            addr[++k] = (segs[i].top & ~3u) >> 2;
            cfg[k] = PMP_TOR | PMP_R |
                     ((segs[i].attr == PLAT_SEG_CODE) ? PMP_X : PMP_W);
        }
    }

    /* Entries 0..3 live in pmpcfg0; entry 4 shares pmpcfg1 with entry 5 (the
     * trap vector), so that byte's neighbour has to be preserved. */
    cfg0 = cfg[0] | (cfg[1] << 8) | (cfg[2] << 16) | (cfg[3] << 24);
    __asm__ volatile("csrw pmpaddr0, %0" : : "r"(addr[0]));
    __asm__ volatile("csrw pmpaddr1, %0" : : "r"(addr[1]));
    __asm__ volatile("csrw pmpaddr2, %0" : : "r"(addr[2]));
    __asm__ volatile("csrw pmpaddr3, %0" : : "r"(addr[3]));
    __asm__ volatile("csrw pmpaddr4, %0" : : "r"(addr[4]));
    __asm__ volatile("csrw pmpcfg0, %0" : : "r"(cfg0));
    __asm__ volatile("csrr %0, pmpcfg1" : "=r"(cfg1));
    cfg1 = (cfg1 & ~0xffu) | cfg[4];
    __asm__ volatile("csrw pmpcfg1, %0" : : "r"(cfg1));
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

uint32_t plat_frame_get_user_sp(const regs_t *r)
{
    return r->sp;
}

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

__attribute__((weak)) int plat_board_task_protect(const plat_seg_t *segs, int n)
{
    (void)segs; (void)n;
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

/* Non-blocking variant, and the one spin_lock() actually calls: it retries
 * around plat_flash_park_poll() and the stuck-lock panic, so a lock wait can
 * answer a flash park request.  It was missing here and the weak default in
 * kernel/cpu.c - "one CPU, the lock is never contended, return 1" - was what
 * every RV32 build linked.  On atk-dnr2350am-rv32-smp that made proc_lock a no-op: both
 * harts picked the same ST_READY process (the shell banner printed twice, and
 * `[sched] cpu 0 idle: c0=1/1 c1=1/1` in the "no runnable process" panic),
 * and the flash-park handshake raced against itself.  amoswap stores 1
 * unconditionally, so a failed attempt leaves the lock exactly as it found it
 * and only the returned value says whether this hart owns it now. */
int plat_spin_try_acquire(volatile int *v)
{
    uint32_t got;

    __asm__ volatile("amoswap.w.aq %0, %1, (%2)"
                     : "=r"(got)
                     : "r"(1), "r"(v)
                     : "memory");
    return got == 0;
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
