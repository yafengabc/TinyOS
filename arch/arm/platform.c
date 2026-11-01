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
 * There is no MMU and no MPU in this port yet: all addresses are physical
 * and task windows come from the bump allocator in kernel/process.c.  User
 * tasks run in Thread mode on PSP but *privileged* (CONTROL.nPRIV = 0),
 * because turning nPRIV on has to happen as the last step before the
 * exception return and any interrupt taken in that window would run the
 * handler unprivileged.  Moving the switch into PendSV, which is the
 * lowest-priority exception and therefore cannot be preempted, is the
 * follow-up that MPU-protected user mode will need.
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

/* Called from every fault stub.  Returns the frame to resume with. */
regs_t *arm_handle_fault(regs_t *f)
{
    if (frame_is_user(f)) {
        kprintf("\n[pid %d] %s at 0x%08x\n", process_current_pid(),
                vec_name(f->vec), f->pc);
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

       PendSV is parked at the lowest priority for the future MPU work,
       where the unprivileged switch has to happen at the last moment. */
    mmio_write32(ARM_SHPR3, (ARM_PRIO_SYSTICK << 24) | (ARM_PRIO_PENDSV << 16));
    mmio_write32(ARM_SHPR2, ARM_PRIO_SVC << 24);

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

/* No MPU yet: user mode can reach the whole address space.  The hook is
 * here so the MPU work has one place to land, and because the scheduler
 * already calls it on every switch. */
void plat_task_protect(uint32_t win_base, uint32_t wx, uint32_t brk,
                       uint32_t stack_low, uint32_t stack_top)
{
    (void)win_base;
    (void)wx;
    (void)brk;
    (void)stack_low;
    (void)stack_top;
}

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


