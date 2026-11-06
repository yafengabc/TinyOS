/*
 * TinyOS Cortex-M3 (ARMv7-M) entry point and exception handlers.
 *
 * Board: QEMU mps2-an385
 *   0x00000000  the image; word 0 = initial SP, word 1 = reset vector
 *   0x20000000  4 MiB SRAM (.data, .bss, kernel stack, user windows)
 *
 * Exception model (ARMv7-M):
 *   - On entry the core pushes {r0,r1,r2,r3,r12,lr,pc,xpsr} onto the stack
 *     the interrupted context used - PSP in Thread mode, MSP in Handler
 *     mode - and loads LR with EXC_RETURN, which says how to return.
 *   - Thread mode runs user code on PSP; Handler mode always runs on MSP,
 *     so a read of sp inside a handler always means MSP.
 *
 * Every stub parks EXC_RETURN in r3 *before* branching, because "bl"
 * overwrites LR with the return address.  arm_trap_enter then builds an
 * 80-byte regs_t on MSP (arch/arm/regs.h):
 *
 *      [sp+ 0..31]  saved r4..r11
 *      [sp+32]      EXC_RETURN (from r3)
 *      [sp+36]      exception number (from r1)
 *      [sp+40]      hardware frame address (PSP for a user trap)
 *      [sp+44]      MSP to restore on return
 *      [sp+48..79]  copy of the hardware frame (r0..r3,r12,lr,pc,xpsr)
 */

    .syntax unified
    /* The CPU is chosen by the build's -mcpu= flag (cortex-m3 for the MPS2
       board, cortex-m0plus for the RP2040), so do not pin it here. */
    .thumb

/* Kernel stack per CPU.  Must equal KSTACK_BYTES in kernel/cpu.h, which is
 * what cpu_init() checks the pool against at boot (a mismatch does not fail
 * to link, it puts the overflow guard word somewhere else).  ARM uses half
 * the RV32/x86 size: the RP2350 has 128 KiB for the whole kernel, and two
 * 32 KiB stacks would not fit next to the 60 KiB exec arena. */
#define ARM_KSTACK_BYTES 16384

#ifndef TOS_MAX_CPUS
#define TOS_MAX_CPUS 1
#endif

/* -------------------------------------------------------------------- */

    .section .vectors, "a"
    .align 2
    .globl __vector_table
__vector_table:
    .word   __kernel_stack_top      /* 0  initial MSP                   */
    .word   _start                  /* 1  reset                         */
    .word   arm_default_handler     /* 2  NMI                           */
    .word   arm_hardfault           /* 3  HardFault                     */
    .word   arm_memmanage           /* 4  MemManage                     */
    .word   arm_busfault            /* 5  BusFault                      */
    .word   arm_usagefault          /* 6  UsageFault                    */
    /* ARMv8-M (Cortex-M33 / RP2350) makes slot 7 SecureFault; ARMv7-M and
       ARMv6-M leave it reserved.  A zero here would send a TrustZone fault
       straight to address 0 and lock the core with no diagnostic at all. */
    .word   arm_default_handler     /* 7  SecureFault (M33 only)        */
    .word   0, 0, 0                 /* 8..10                            */
    .word   arm_svc                 /* 11 SVCall                        */
    .word   arm_default_handler     /* 12 DebugMon                      */
    .word   0                       /* 13                               */
    .word   arm_pendsv              /* 14 PendSV                        */
    .word   arm_systick             /* 15 SysTick                       */

/* External interrupts (NVIC IRQ n vector 16+n).  Only USBCTRL (IRQ 14,
 * slot 30) is wired up: the RP2350 USB controller is the one peripheral
 * whose events cannot wait for the 1 ms tick, because a SET_ADDRESS status
 * stage has to be answered and the new device address applied within
 * microseconds of the host acknowledging it.  Everything else lands in
 * arm_default_handler, which parks the core with the vector number visible
 * so a stray IRQ is diagnosable instead of silently vectoring to zero. */
    .word   arm_default_handler     /* 16  IRQ 0                        */
    .word   arm_default_handler     /* 17  IRQ 1                        */
    .word   arm_default_handler     /* 18  IRQ 2                        */
    .word   arm_default_handler     /* 19  IRQ 3                        */
    .word   arm_default_handler     /* 20  IRQ 4                        */
    .word   arm_default_handler     /* 21  IRQ 5                        */
    .word   arm_default_handler     /* 22  IRQ 6                        */
    .word   arm_default_handler     /* 23  IRQ 7                        */
    .word   arm_default_handler     /* 24  IRQ 8                        */
    .word   arm_default_handler     /* 25  IRQ 9                        */
    .word   arm_default_handler     /* 26  IRQ 10                       */
    .word   arm_default_handler     /* 27  IRQ 11                       */
    .word   arm_default_handler     /* 28  IRQ 12                       */
    .word   arm_default_handler     /* 29  IRQ 13                       */
    .word   arm_usbctrl_irq         /* 30  IRQ 14  USBCTRL              */
    .word   arm_default_handler     /* 31  IRQ 15                       */
    .word   arm_default_handler     /* 32  IRQ 16                       */
    .word   arm_default_handler     /* 33  IRQ 17                       */
    .word   arm_default_handler     /* 34  IRQ 18                       */
    .word   arm_default_handler     /* 35  IRQ 19                       */
    .word   arm_default_handler     /* 36  IRQ 20                       */
    .word   arm_default_handler     /* 37  IRQ 21                       */
    .word   arm_default_handler     /* 38  IRQ 22                       */
    .word   arm_default_handler     /* 39  IRQ 23                       */
    .word   arm_default_handler     /* 40  IRQ 24                       */
    .word   arm_default_handler     /* 41  IRQ 25                       */
    .word   arm_default_handler     /* 42  IRQ 26                       */
    .word   arm_default_handler     /* 43  IRQ 27                       */
    .word   arm_default_handler     /* 44  IRQ 28                       */
    .word   arm_default_handler     /* 45  IRQ 29                       */
    .word   arm_default_handler     /* 46  IRQ 30                       */
    .word   arm_default_handler     /* 47  IRQ 31                       */

/* Offsets must match regs_t in arch/arm/regs.h. */
    .equ FR_R4,    0
    .equ FR_EXC,  32
    .equ FR_VEC,  36
    .equ FR_HW,   40
    .equ FR_TSP,  44
    .equ FR_R0,   48
    .equ FR_SIZE, 80

/* -------------------------------------------------------------------- */

    .section .text.entry, "ax"
    .globl _start
    .thumb_func
_start:
    movs    r0, #0
    msr     control, r0
    isb

    ldr     r0, =__data_start
    ldr     r1, =__data_end
    ldr     r2, =__data_load
1:  cmp     r0, r1
    bhs     2f
    /* Post-indexed addressing is Thumb-2 only; ARMv6-M (Cortex-M0+) needs
       the increment spelled out.  The same form is valid on the M3. */
    ldr     r3, [r2]
    str     r3, [r0]
    adds    r2, #4
    adds    r0, #4
    b       1b
2:  ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    movs    r3, #0
3:  cmp     r0, r1
    bhs     4f
    str     r3, [r0]
    adds    r0, #4
    b       3b
4:  bl      kmain

park:
    wfi
    b       park

/* -------------------------------------------------------------------- */
/* arm_trap_enter: r1 = vector, r3 = EXC_RETURN.  Returns regs_t in r0.  */

    .thumb_func
arm_trap_enter:
    /* Build the 80-byte regs_t so that its layout matches regs.h exactly:
       the saved r4..r11 sit at +0, the bookkeeping words at +32 and the
       copy of the hardware frame at +48.  The reserve has to come first:
       "push" after it puts the saved registers at the BOTTOM of the block
       (r0+0), while pushing first would place them at r0+48 and leave the
       stale kernel stack at the offset arm_resume pops from. */
    sub     sp, #80                 /* whole regs_t frame (8-byte aligned) */
    mov     r0, sp
    /* Save r4-r11 explicitly.  ARMv6-M (Cortex-M0+) cannot push the high
       registers in one PUSH, nor store them with an immediate offset, so the
       high registers are moved through a low scratch first.  The same code is
       valid on the Cortex-M3, so a single path serves every board. */
    str     r4,  [r0, #FR_R4 + 0]
    str     r5,  [r0, #FR_R4 + 4]
    str     r6,  [r0, #FR_R4 + 8]
    str     r7,  [r0, #FR_R4 + 12]
    /* r3 = EXC_RETURN and r1 = vector are live, so record them before r1 is
       reused as the high-register scratch. */
    str     r3,  [r0, #FR_EXC]
    str     r1,  [r0, #FR_VEC]
    mov     r1,  r8
    str     r1,  [r0, #FR_R4 + 16]
    mov     r1,  r9
    str     r1,  [r0, #FR_R4 + 20]
    mov     r1,  r10
    str     r1,  [r0, #FR_R4 + 24]
    mov     r1,  r11
    str     r1,  [r0, #FR_R4 + 28]

    /* Hardware frame address: PSP for a Thread-mode trap, otherwise the
       words the core stacked just below our frame.  r1 still holds the
       saved r11 here, NOT the frame size - do not add it.  ARMv6-M cannot
       add an immediate larger than 7, so load FR_SIZE into r2 first (r2
       is free once the TST above has been taken). */
    ldr     r3, [r0, #FR_EXC]
    movs    r2, #4
    tst     r3, r2
    beq     1f
    mrs     r3, psp                 /* Thread: hardware frame on PSP    */
    b       2f
1:  movs    r2, #FR_SIZE
    movs    r3, r0
    adds    r3, r2                  /* Handler: frame sits under ours    */
2:  str     r3, [r0, #FR_HW]

    movs    r1, r0
    adds    r1, #FR_SIZE
    str     r1, [r0, #FR_TSP]

    /* Copy the hardware frame into the regs_t view. */
    ldr     r4, [r3, #0]
    ldr     r5, [r3, #4]
    ldr     r6, [r3, #8]
    ldr     r7, [r3, #12]
    str     r4, [r0, #FR_R0 + 0]
    str     r5, [r0, #FR_R0 + 4]
    str     r6, [r0, #FR_R0 + 8]
    str     r7, [r0, #FR_R0 + 12]
    ldr     r4, [r3, #16]
    ldr     r5, [r3, #20]
    ldr     r6, [r3, #24]
    ldr     r7, [r3, #28]
    str     r4, [r0, #FR_R0 + 16]
    str     r5, [r0, #FR_R0 + 20]
    str     r6, [r0, #FR_R0 + 24]
    str     r7, [r0, #FR_R0 + 28]

    bx      lr

/* -------------------------------------------------------------------- */
/* arm_resume: r0 = regs_t.  Switch to the frame's task stack, write the  */
/* hardware-frame copy back, restore r4-r11 and MSP, and return from the  */
/* exception.                                                             */

    .globl arm_resume
    .thumb_func
arm_resume:
    /* The frame arrives in r0 (arm_trap_enter and every C helper returns
       it there, and the svc task-start path keeps it in r0).  It must stay
       in a LOW register: ARMv6-M cannot use a high register as an LDR/STR
       base, and r0 already is one - the M3-only rewrite moved it to r12
       for indexed addressing, but on the M0+ that copy is backwards and
       would clobber the frame pointer with garbage. */
    ldr     r1, [r0, #FR_HW]
    cmp     r1, #0
    beq     9f

    /* A frame that goes back to Thread mode on PSP (EXC_RETURN bit 2) must
       also switch PSP: the exception return pops the register frame from
       PSP, which still belongs to the task we are leaving because the
       switch happened inside another exception.  The saved hardware frame
       address is the PSP the core must pop.  Frames returning to Handler
       mode or to Thread mode on MSP keep the current PSP. */
    ldr     r3, [r0, #FR_EXC]
    movs    r2, #4
    tst     r3, r2
    beq     8f
    msr     psp, r1
8:
    ldr     r2, [r0, #FR_R0 + 0]
    str     r2, [r1, #0]
    ldr     r2, [r0, #FR_R0 + 4]
    str     r2, [r1, #4]
    ldr     r2, [r0, #FR_R0 + 8]
    str     r2, [r1, #8]
    ldr     r2, [r0, #FR_R0 + 12]
    str     r2, [r1, #12]
    ldr     r2, [r0, #FR_R0 + 16]
    str     r2, [r1, #16]
    ldr     r2, [r0, #FR_R0 + 20]
    str     r2, [r1, #20]
    ldr     r2, [r0, #FR_R0 + 24]
    str     r2, [r1, #24]
    ldr     r2, [r0, #FR_R0 + 28]
    str     r2, [r1, #28]
9:
    ldr     r2, [r0, #FR_EXC]
    mov     lr, r2
    ldr     r3, [r0, #FR_TSP]       /* r3 = task stack to resume          */
    mov     sp, r0                  /* sp = frame base; r4-r11 at +0..28 */
    ldr     r4,  [sp, #FR_R4 + 0]
    ldr     r5,  [sp, #FR_R4 + 4]
    ldr     r6,  [sp, #FR_R4 + 8]
    ldr     r7,  [sp, #FR_R4 + 12]
    ldr     r1,  [sp, #FR_R4 + 16]
    mov     r8,  r1
    ldr     r1,  [sp, #FR_R4 + 20]
    mov     r9,  r1
    ldr     r1,  [sp, #FR_R4 + 24]
    mov     r10, r1
    ldr     r1,  [sp, #FR_R4 + 28]
    mov     r11, r1
    mov     sp, r3                   /* sp = task stack to resume          */

    /* Returning to Thread mode on PSP: make it unprivileged.  Thread mode
       on MSP (a tick that interrupted kmain before the first task) keeps
       its privileges - it is still kernel code.  CONTROL.nPRIV is ignored
       in Handler mode, so writing it here, still inside the exception,
       only affects the Thread context the exception return installs.
       Handlers, and any interrupt taken from now on, stay privileged; the
       MPU regions programmed by plat_task_protect() decide what the task
       itself can touch.  SPSEL comes from EXC_RETURN, and the core has no
       FPU, so the value written is just nPRIV. */
    movs    r2, #4
    mov     r1, lr
    tst     r1, r2                  /* bit 2: Thread mode on PSP?       */
    beq     7f
    movs    r2, #1                  /* CONTROL.nPRIV = 1                */
    msr     control, r2
    isb
    /* ARM has no MPIE: exception entry/exit does NOT save or restore
       PRIMASK.  sched_switch() masks interrupts around its run-queue scan
       and returns with them still masked, because on RV32 `mret` restores
       MIE from MPIE.  On Cortex-M that mask would be permanent: the very
       next svc from user code is escalated straight to HardFault, with
       HFSR.FORCED set and CFSR clear - exactly the RP2350 shell crash.
       Returning to user Thread mode must leave interrupts enabled. */
    cpsie   i
7:  bx      lr

/* -------------------------------------------------------------------- */
/* arm_enter_user: r0 = regs_t of a task that has never run.             */
/*                                                                       */
/* EXC_RETURN is only honoured on the way out of an exception, so this   */
/* publishes the frame and raises svc: arm_svc notices the pending start */
/* and resumes the task frame instead of dispatching a syscall.          */
/*                                                                       */
/* One slot per core.  A single shared word looked fine while only the   */
/* boot CPU ever started a task, but a secondary core uses this path every
 * time it picks a task up from idle, and in that window the other core's
 * syscall would find the word set and "start" that task too - two cores
 * running one task, each writing the other's saved frame.  Measured on
 * the RP2350 as the shell dying with kernel registers in its r4-r11.     */
    .section .data
    .align 2
arm_pending_frame:
    .word   0
    .word   0

/* r\reg = &arm_pending_frame[this core].  MSP at exception entry lies in
 * this core's own kernel stack, and the blocks are laid out back to back
 * below __kernel_stack_top, so one comparison names the core without
 * asking a board register: SIO CPUID exists on both RP2 parts but this
 * file is also assembled for boards that have no such thing. */
    .macro pending_slot reg
    ldr     \reg, =arm_pending_frame
    ldr     r2, =__kernel_stack_top - ARM_KSTACK_BYTES
    cmp     sp, r2
    bhi     9f
    adds    \reg, \reg, #4
9:
    .endm

    .section .text.startuser, "ax"
    .globl  arm_enter_user
    .thumb_func
arm_enter_user:
    pending_slot r1
    str     r0, [r1]
    /* SVC with PRIMASK set escalates to HardFault (the exception is masked
     * out by priority masking, so the core reports "forced"), and
     * cpu_take_task() deliberately returns with interrupts masked because
     * its caller may be about to go idle.  The first task a secondary core
     * starts therefore arrives here with PRIMASK=1 and the SVC never
     * dispatches: measured on the RP2350 as HardFault at the "not reached"
     * spin below, with CFSR completely clear.  Enable interrupts first -
     * the exception entry masks them again for the handler. */
    cpsie   i
    svc     #0
    /* Not reached. */
1:  b       1b

    /* -------------------------------------------------------------------- */
/* Exception stubs.  EXC_RETURN goes to r3 before anything clobbers LR.  */

    /* Tail jump to arm_resume.  The stubs live in .text while arm_resume
       is in .text.entry, so the distance is whatever the linker makes it
       (tens of KB once the C sections sit in between) - far beyond the
       +/-2 KiB of the ARMv6-M 16-bit B.  Load the address and bx instead;
       BL would work too (arm_resume replaces lr from the frame), but this
       says "tail call" plainly.  r3 is scratch at every site below. */
    .macro tail_resume
    ldr     r3, =arm_resume
    bx      r3
    .endm

    .thumb_func
arm_svc:
    mov     r3, lr
    movs    r1, #11
    bl      arm_trap_enter          /* r0 = kernel frame                */

    pending_slot r1
    ldr     r3, [r1]
    cmp     r3, #0
    beq     1f

    /* Start a new task: overwrite the frame we are about to resume with
       the task's own, and switch to the user stack. */
    movs    r4, #0
    str     r4, [r1]
    ldr     r4, [r3, #FR_EXC]
    str     r4, [r0, #FR_EXC]
    ldr     r4, [r3, #FR_HW]
    str     r4, [r0, #FR_HW]
    msr     psp, r4
    /* r4-r11 too.  The frame arm_trap_enter just built holds *this* call
       site's callee-saved registers - on a secondary core that means the
       launch path's, e.g. SIO_BASE in r4 and VTOR in r7.  A task may start
       with any values there (every function saves what it uses), but handing
       it kernel pointers is how the shell ended up dereferencing an image
       offset: measured on the RP2350, r5 = 0x173 and r6 = 0xe000ed94 the
       moment core 1 ran a task.  proc_create() gave the task its own
       zeroed r4-r11 for a reason. */
    ldr     r4, [r3, #FR_R4 + 0]
    str     r4, [r0, #FR_R4 + 0]
    ldr     r4, [r3, #FR_R4 + 4]
    str     r4, [r0, #FR_R4 + 4]
    ldr     r4, [r3, #FR_R4 + 8]
    str     r4, [r0, #FR_R4 + 8]
    ldr     r4, [r3, #FR_R4 + 12]
    str     r4, [r0, #FR_R4 + 12]
    ldr     r4, [r3, #FR_R4 + 16]
    str     r4, [r0, #FR_R4 + 16]
    ldr     r4, [r3, #FR_R4 + 20]
    str     r4, [r0, #FR_R4 + 20]
    ldr     r4, [r3, #FR_R4 + 24]
    str     r4, [r0, #FR_R4 + 24]
    ldr     r4, [r3, #FR_R4 + 28]
    str     r4, [r0, #FR_R4 + 28]
    ldr     r4, [r3, #FR_R0 + 0]
    str     r4, [r0, #FR_R0 + 0]
    ldr     r4, [r3, #FR_R0 + 4]
    str     r4, [r0, #FR_R0 + 4]
    ldr     r4, [r3, #FR_R0 + 8]
    str     r4, [r0, #FR_R0 + 8]
    ldr     r4, [r3, #FR_R0 + 12]
    str     r4, [r0, #FR_R0 + 12]
    ldr     r4, [r3, #FR_R0 + 16]
    str     r4, [r0, #FR_R0 + 16]
    ldr     r4, [r3, #FR_R0 + 20]
    str     r4, [r0, #FR_R0 + 20]
    ldr     r4, [r3, #FR_R0 + 24]
    str     r4, [r0, #FR_R0 + 24]
    ldr     r4, [r3, #FR_R0 + 28]
    str     r4, [r0, #FR_R0 + 28]
    /* Thread mode on PSP comes from the task's EXC_RETURN (0xFFFFFFFD):
       exception return restores CONTROL.SPSEL from it.  arm_resume also
       sets CONTROL.nPRIV on the way out, so the task starts unprivileged
       with only the MPU regions plat_task_protect() installed. */
    tail_resume
1:
    bl      syscall_dispatch
    tail_resume

    .thumb_func
arm_systick:
    mov     r3, lr
    movs    r1, #15
    bl      arm_trap_enter
    bl      arm_handle_tick
    tail_resume

/* A plain peripheral interrupt.  BL writes its return address into LR, so
 * the EXC_RETURN value the core left there has to be banked first - without
 * this the BX LR below branches back to itself and the handler spins
 * forever inside the interrupt, with no fault and no output.
 *
 * Two words are pushed, not one, to keep SP 8-byte aligned for the AAPCS
 * call as required by the EABI.  The restored r0 is irrelevant (the core
 * already stacked the real one); it exists only as padding.
 *
 * The vector table is shared by every ARM board, so this stub dispatches
 * through a function pointer instead of naming a board symbol; a board
 * with no USB controller simply leaves the pointer NULL. */
    .thumb_func
arm_usbctrl_irq:
    push    {r0, lr}
    bl      arm_usbctrl_irq_c
    pop     {r0, pc}

    .thumb_func
arm_hardfault:
    mov     r3, lr
    movs    r1, #3
    b       arm_fault

    .thumb_func
arm_memmanage:
    mov     r3, lr
    movs    r1, #4
    b       arm_fault

    .thumb_func
arm_busfault:
    mov     r3, lr
    movs    r1, #5
    b       arm_fault

    .thumb_func
arm_usagefault:
    mov     r3, lr
    movs    r1, #6
    b       arm_fault

    .thumb_func
arm_pendsv:
    mov     r3, lr
    movs    r1, #14
    b       arm_fault

    .thumb_func
arm_default_handler:
    mov     r3, lr
    movs    r1, #0

arm_fault:
    bl      arm_trap_enter
    bl      arm_handle_fault
    tail_resume

/* -------------------------------------------------------------------- */
/* Entry for a secondary core.
 *
 * Nothing jumps here from the reset vector: on a Cortex-M the second core
 * either starts in its own boot-ROM wait loop (RP2350, and it must be kicked
 * through the SIO FIFO) or does not exist at all.  The board's launch
 * sequence therefore passes this address, a stack pointer and a vector
 * table to the ROM, which is what the values below re-assert.
 *
 * MSP and VTOR are per-core registers, so a secondary that sets them here
 * shares the one vector table in flash with core 0 and still builds its
 * trap frames on its own stack.  The scheduler work is all in C.
 *
 * Its own section: the literals below have to stay inside the Thumb-1
 * 1 KiB literal range on the M0+ build, which never calls this but still
 * assembles it. */
    .section .text.core1, "ax"
    .globl  arm_core1_entry
    .thumb_func
arm_core1_entry:
    cpsid   i
    ldr     r0, =__kernel_stack_top - ARM_KSTACK_BYTES
    mov     sp, r0
    ldr     r0, =__vector_table
    ldr     r1, =0xE000ED08                  /* SCB->VTOR */
    str     r0, [r1]
    dsb
    isb
    bl      cpu_secondary_main               /* never returns */
1:  b       1b
    .ltorg

/* -------------------------------------------------------------------- */

    .section .bss
    .align 3
    .globl __kernel_stack
__kernel_stack:
    /* One stack per CPU: core N gets
     * [__kernel_stack_top - (N+1)*16K, __kernel_stack_top - N*16K). */
    .space ARM_KSTACK_BYTES * TOS_MAX_CPUS
    .globl __kernel_stack_top
__kernel_stack_top:

    .ltorg
