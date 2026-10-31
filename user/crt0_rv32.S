/*
 * User program entry point, RV32.
 *
 * The kernel hands over with sp pointing at [argc][argv...][NULL], 16-byte
 * aligned, in the task's own window.
 */
    .section .text.entry, "ax"
    .globl _start
_start:
    lw      a0, 0(sp)              /* argc                          */
    addi    a1, sp, 4              /* argv                          */
    call    main
    call    exit
1:  j       1b
