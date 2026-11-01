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
    .cpu    cortex-m3
    .thumb

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
    .word   0, 0, 0, 0              /* 7..10                            */
    .word   arm_svc                 /* 11 SVCall                        */
    .word   arm_default_handler     /* 12 DebugMon                      */
    .word   0                       /* 13                               */
    .word   arm_pendsv              /* 14 PendSV                        */
    .word   arm_systick             /* 15 SysTick                       */

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
    ldr     r3, [r2], #4
    str     r3, [r0], #4
    b       1b
2:  ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    movs    r3, #0
3:  cmp     r0, r1
    bhs     4f
    str     r3, [r0], #4
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
    sub     sp, #48                 /* bookkeeping + hardware copy       */
    push    {r4-r11}                /* 32 bytes; 80 total, 8-byte aligned */
    mov     r0, sp

    str     r3, [r0, #FR_EXC]
    str     r1, [r0, #FR_VEC]

    /* Hardware frame address: PSP for a Thread-mode trap, otherwise the
       words the core stacked just below our frame. */
    ldr     r3, [r0, #FR_EXC]
    movs    r2, #4
    tst     r3, r2
    beq     1f
    mrs     r3, psp                 /* Thread: hardware frame on PSP    */
    b       2f
1:  add     r3, r0, #FR_SIZE       /* Handler: it sits under our frame */
2:  str     r3, [r0, #FR_HW]

    add     r1, r0, #FR_SIZE
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
    mov     r12, r0
    ldr     r1, [r12, #FR_HW]
    cbz     r1, 9f

    /* A frame that goes back to Thread mode on PSP (EXC_RETURN bit 2) must
       also switch PSP: the exception return pops the register frame from
       PSP, which still belongs to the task we are leaving because the
       switch happened inside another exception.  The saved hardware frame
       address is the PSP the core must pop.  Frames returning to Handler
       mode or to Thread mode on MSP keep the current PSP. */
    ldr     r3, [r12, #FR_EXC]
    tst     r3, #4
    beq     8f
    msr     psp, r1
8:
    ldr     r2, [r12, #FR_R0 + 0]
    str     r2, [r1, #0]
    ldr     r2, [r12, #FR_R0 + 4]
    str     r2, [r1, #4]
    ldr     r2, [r12, #FR_R0 + 8]
    str     r2, [r1, #8]
    ldr     r2, [r12, #FR_R0 + 12]
    str     r2, [r1, #12]
    ldr     r2, [r12, #FR_R0 + 16]
    str     r2, [r1, #16]
    ldr     r2, [r12, #FR_R0 + 20]
    str     r2, [r1, #20]
    ldr     r2, [r12, #FR_R0 + 24]
    str     r2, [r1, #24]
    ldr     r2, [r12, #FR_R0 + 28]
    str     r2, [r1, #28]
9:
    ldr     lr, [r12, #FR_EXC]
    ldr     r0, [r12, #FR_TSP]
    mov     sp, r12
    pop     {r4-r11}
    mov     sp, r0
    bx      lr

/* -------------------------------------------------------------------- */
/* arm_enter_user: r0 = regs_t of a task that has never run.             */
/*                                                                       */
/* EXC_RETURN is only honoured on the way out of an exception, so this   */
/* publishes the frame and raises svc: arm_svc notices the pending start */
/* and resumes the task frame instead of dispatching a syscall.          */

    .section .data
    .align 2
arm_pending_frame:
    .word   0

    .section .text
    .globl arm_enter_user
    .thumb_func
arm_enter_user:
    ldr     r1, =arm_pending_frame
    str     r0, [r1]
    svc     #0
    /* Not reached. */
1:  b       1b

/* -------------------------------------------------------------------- */
/* Exception stubs.  EXC_RETURN goes to r3 before anything clobbers LR.  */

    .thumb_func
arm_svc:
    mov     r3, lr
    movs    r1, #11
    bl      arm_trap_enter          /* r0 = kernel frame                */

    ldr     r2, =arm_pending_frame
    ldr     r3, [r2]
    cbz     r3, 1f

    /* Start a new task: overwrite the frame we are about to resume with
       the task's own, and switch to the user stack. */
    movs    r4, #0
    str     r4, [r2]
    ldr     r4, [r3, #FR_EXC]
    str     r4, [r0, #FR_EXC]
    ldr     r4, [r3, #FR_HW]
    str     r4, [r0, #FR_HW]
    msr     psp, r4
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
       exception return restores CONTROL.SPSEL from it, so nothing needs to
       be written here.  The task runs privileged, like the rest of this
       flat-model port - see the note in arch/arm/platform.c. */
    b       arm_resume
1:
    bl      syscall_dispatch
    b       arm_resume

    .thumb_func
arm_systick:
    mov     r3, lr
    movs    r1, #15
    bl      arm_trap_enter
    bl      arm_handle_tick
    b       arm_resume

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
    b       arm_resume

/* -------------------------------------------------------------------- */

    .section .bss
    .align 3
    .globl __kernel_stack
__kernel_stack:
    .space 32768
    .globl __kernel_stack_top
__kernel_stack_top:

    .ltorg
