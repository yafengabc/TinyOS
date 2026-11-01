#include "platform.h"
#include "board.h"
#include "scs.h"
#include "regs.h"
#include "console.h"
#include "string.h"
#include "process.h"

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
        kprintf("\n[pid %d] %s at 0x%08x", process_current_pid(),
                vec_name(f->vec), f->pc);
        if (addr)
            kprintf(" (address 0x%08x)", addr);
        kprintf("\n");
        return process_exit(f, 139);
    }

    kprintf("\n*** ARM EXCEPTION ***\n");
    kprintf("%s: pc=0x%08x lr=0x%08x xpsr=0x%08x\n",
            vec_name(f->vec), f->pc, f->lr, f->xpsr);
    kprintf("r0=0x%08x r1=0x%08x r2=0x%08x r3=0x%08x\n",
            f->r0, f->r1, f->r2, f->r3);
    kprintf("hw=0x%08x exc=0x%08x sp=0x%08x\n", f->hw, f->exc_return, f->kern_sp);
    plat_panic_halt();
    return f;
}

/* Called from the SysTick stub. */
regs_t *arm_handle_tick(regs_t *f)
{
    plat_timer_tick();
    process_tick(f);
    return process_resched(f);
}

/* ---- boot ------------------------------------------------------------ */

void plat_early_init(void)
{
    plat_board_early();
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
    /* Turn the configurable faults on: without these bits an MPU violation
       is escalated to HardFault and `prot` cannot say why the task died.
       The Cortex-M0+ has no configurable fault status (everything is
       HardFault), so this is M3-only. */
    mmio_write32(ARM_SHCSR, ARM_SHCSR_MEMFAULTENA |
                            ARM_SHCSR_BUSFAULTENA |
                            ARM_SHCSR_USGFAULTENA);

    /* MPU on.  No region covers kernel RAM or the MMIO, so privileged code
       reaches them through the default map (PRIVDEFENA); unprivileged code
       gets the default map denied and only sees what plat_task_protect()
       programs for the running task.  Enabling it here, before any task
       exists, means user mode can never start unprotected.
       The Cortex-M0+ has no MPU, so user tasks run unprivileged but with
       the whole flat address space visible - the same shape as the x86
       port (see plat_task_protect below). */
    mmio_write32(ARM_MPU_CTRL, ARM_MPU_CTRL_ENABLE | ARM_MPU_CTRL_PRIVDEFENA);
    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
#endif

    plat_board_init();
}

/* ---- interrupts ------------------------------------------------------ */

static int irq_ready;

void plat_irq_enable(void)
{
    /* The boards leave their tick source masked until the scheduler is up;
       this is the first safe moment to unmask it. */
    if (!irq_ready) {
        irq_ready = 1;
        plat_timer_irq_unmask();
    }
    __asm__ volatile("cpsie i" ::: "memory");
}

void plat_irq_disable(void)
{
    __asm__ volatile("cpsid i" ::: "memory");
}

void plat_cpu_idle(void)
{
    __asm__ volatile("cpsie i; wfi" ::: "memory");
}

/* ---- user mode ------------------------------------------------------- */

#ifndef TOS_ARM_M0P

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

/* Per-task MPU.  The running task gets:
 *
 *   0      RW, XN  the whole window       data, heap, stack
 *   1..6   RO       [win_base, wx) in      code and read-only data
 *                   aligned power-of-two   (executable: no XN)
 *                   blocks
 *   7      --       the hole under the stack  stack overflow guard
 *
 * Region numbers are priority: a higher number wins where two overlap, so
 * the read-only blocks sit on top of the window region and the guard on
 * top of everything.  Because region 0 is execute-never, only the blocks
 * that cover [win_base, wx) can be executed: that is w^X, the same shape
 * rv32 gets from PMP (code RX, data/heap/stack RW with X off).  Everything
 * outside the window - kernel RAM, MMIO, other tasks - matches no region
 * and is denied to user mode (the kernel still reaches it through
 * PRIVDEFENA).
 *
 * What an ARMv7-M MPU cannot express, and the rv32 PMP can: a boundary at
 * an arbitrary byte.  Regions are naturally aligned powers of two, so the
 * heap is not cut at `brk` (a write just past the break survives until
 * sbrk() covers it), and the hole below the stack is one aligned block
 * rather than everything from brk to stack_low.  Code is read-only down to
 * the 32-byte minimum region and user.ld aligns the writable segment to
 * 32 bytes, so the split at wx is exact.
 *
 * mpu_region/mpu_disable are not atomic with respect to the tick.  That is
 * fine: a handler taken mid-update runs privileged and access to RAM and
 * MMIO does not depend on region 0, and process_tick only reschedules on a
 * user frame. */
void plat_task_protect(uint32_t win_base, uint32_t wx, uint32_t brk,
                       uint32_t stack_low, uint32_t stack_top)
{
    uint32_t addr, n;

    (void)brk;
    (void)stack_top;

    /* wx is 32-byte aligned by user.ld; round up defensively so a stale
       script cannot turn the tail of .text into non-executable memory.  At
       worst a few bytes of .data become read-only. */
    wx = (wx + ARM_MPU_MIN_SIZE - 1u) & ~(ARM_MPU_MIN_SIZE - 1u);

    /* The window is the board's PLAT_APP_WINDOW and every slot starts on a
       multiple of it, so the base is aligned for a region of that size. */
    mpu_region(0, win_base, PLAT_APP_WINDOW,
               ARM_MPU_AP_FULL | ARM_MPU_SRAM | ARM_MPU_XN);

    /* Cover the read-only part of the image with the fewest aligned blocks
       the eight-region budget allows, starting at the smallest region an
       ARMv7-M MPU supports. */
    addr = win_base;
    n = 1;
    while (addr < wx && n < 7) {
        uint32_t size = ARM_MPU_MIN_SIZE;

        while ((addr & (2u * size - 1u)) == 0 && addr + 2u * size <= wx)
            size *= 2;
        mpu_region(n++, addr, size, ARM_MPU_AP_RO | ARM_MPU_SRAM);
        addr += size;
    }
    while (n < 7)
        mpu_disable(n++);

    /* A stack that grows past its bottom hits this block: user mode has no
       access, the kernel still does.  Its size is the hole process.c keeps
       between the break and the stack, so no legitimate heap write lands
       here. */
    mpu_region(7, stack_low - PLAT_HEAP_GUARD, PLAT_HEAP_GUARD,
               ARM_MPU_AP_PRIV_RW | ARM_MPU_SRAM | ARM_MPU_XN);

    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
}

#else /* TOS_ARM_M0P: no MPU, user tasks run unprivileged but unprotected. */

void plat_task_protect(uint32_t win_base, uint32_t wx, uint32_t brk,
                       uint32_t stack_low, uint32_t stack_top)
{
    (void)win_base; (void)wx; (void)brk; (void)stack_low; (void)stack_top;
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
       no kernel frame of its own, so start from the top of the kernel
       stack: the next trap overwrites whatever is below it anyway. */
    extern uint8_t __kernel_stack_top[];
    frame->kern_sp = (uint32_t)__kernel_stack_top;
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
    /* MSP comes from the vector table and never changes; there is nothing
       per-task to program. */
    (void)top;
}

void plat_panic_halt(void)
{
    for (;;)
        __asm__ volatile("wfi");
}


