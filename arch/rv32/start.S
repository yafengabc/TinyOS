/*
 * TinyOS RV32 entry point (machine mode only for now).
 *
 * QEMU virt memory map (rv32):
 *   0x80000000  RAM (128 MiB by default)
 *   0x10000000  16550 UART0
 *   0x02000000  CLINT (mtime at +0xBFF8, mtimecmp at +0x4000)
 *
 * P1 goal: get a banner on the serial port and a working periodic timer.
 * User mode (U-mode + PMP) comes next.
 */

    .section .text.entry, "ax"
    .globl _start
_start:
    /* Mask all interrupts until the C side is ready. */
    csrw    mie, zero

    /* Only hart 0 continues; park the rest. */
    bnez    a0, park

    .option push
    .option norelax
    la      sp, __stack_top
    .option pop

    /* Clear .bss */
    .option push
    .option norelax
    la      t0, __bss_start
    la      t1, __bss_end
    .option pop
1:
    bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:
    .option push
    .option norelax
    la      t0, trap_entry
    .option pop
    csrw    mtvec, t0

    call    kmain

park:
    wfi
    j       park

/* -------------------------------------------------------------------- */
/* Trap frame layout shared with C (see arch/rv32/trap.h):              */
/*   0..108   ra,t0-t6,a0-a7,s0-s11                                     */
/*   112      mstatus                                                   */
/*   116      mepc                                                      */
/*   120      mcause                                                    */
/*   124      mtval                                                     */

    .section .text.trap, "ax"
    .align 4
    .globl trap_entry
trap_entry:
    addi    sp, sp, -128
    sw      ra,   0(sp)
    sw      t0,   4(sp)
    sw      t1,   8(sp)
    sw      t2,  12(sp)
    sw      a0,  16(sp)
    sw      a1,  20(sp)
    sw      a2,  24(sp)
    sw      a3,  28(sp)
    sw      a4,  32(sp)
    sw      a5,  36(sp)
    sw      a6,  40(sp)
    sw      a7,  44(sp)
    sw      t3,  48(sp)
    sw      t4,  52(sp)
    sw      t5,  56(sp)
    sw      t6,  60(sp)
    sw      s0,  64(sp)
    sw      s1,  68(sp)
    sw      s2,  72(sp)
    sw      s3,  76(sp)
    sw      s4,  80(sp)
    sw      s5,  84(sp)
    sw      s6,  88(sp)
    sw      s7,  92(sp)
    sw      s8,  96(sp)
    sw      s9, 100(sp)
    sw      s10,104(sp)
    sw      s11,108(sp)

    csrr    t0, mstatus
    sw      t0, 112(sp)
    csrr    t0, mepc
    sw      t0, 116(sp)
    csrr    t0, mcause
    sw      t0, 120(sp)
    csrr    t0, mtval
    sw      t0, 124(sp)

    mv      a0, sp
    call    trap_handler

    /* trap_handler returns the stack pointer to resume on. */
    mv      sp, a0

    lw      t0, 112(sp)
    csrw    mstatus, t0
    lw      t0, 116(sp)
    csrw    mepc, t0

    lw      ra,   0(sp)
    lw      t0,   4(sp)
    lw      t1,   8(sp)
    lw      t2,  12(sp)
    lw      a0,  16(sp)
    lw      a1,  20(sp)
    lw      a2,  24(sp)
    lw      a3,  28(sp)
    lw      a4,  32(sp)
    lw      a5,  36(sp)
    lw      a6,  40(sp)
    lw      a7,  44(sp)
    lw      t3,  48(sp)
    lw      t4,  52(sp)
    lw      t5,  56(sp)
    lw      t6,  60(sp)
    lw      s0,  64(sp)
    lw      s1,  68(sp)
    lw      s2,  72(sp)
    lw      s3,  76(sp)
    lw      s4,  80(sp)
    lw      s5,  84(sp)
    lw      s6,  88(sp)
    lw      s7,  92(sp)
    lw      s8,  96(sp)
    lw      s9, 100(sp)
    lw      s10,104(sp)
    lw      s11,108(sp)

    addi    sp, sp, 128
    mret
