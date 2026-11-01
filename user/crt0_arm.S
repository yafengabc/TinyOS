/*
 * User program entry point, ARM (Cortex-M).
 *
 * The kernel starts a task through an exception return, so the hardware has
 * already restored r0..r3, r12, lr, pc and xpsr when _start runs, and sp is
 * the user stack top.  The initial stack holds [argc][argv...][NULL] below
 * the hardware frame, exactly as on rv32 and i386.
 */
    .syntax unified
    .thumb

    .section .text.entry, "ax"
    .globl _start
    .thumb_func
_start:
    ldr     r0, [sp]                /* argc                           */
    mov     r1, sp
    adds    r1, r1, #4              /* argv                           */
    bl      main
    bl      exit
1:  b       1b

    .ltorg
