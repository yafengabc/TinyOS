/*
 * TinyOS RV32 entry point and trap vector.
 *
 * QEMU virt memory map (rv32):
 *   0x80000000  RAM (128 MiB by default)
 *   0x10000000  16550 UART0
 *   0x02000000  CLINT (mtime at +0xBFF8, mtimecmp at +0x4000)
 *
 * The kernel runs in machine mode; user programs run in U-mode and enter
 * the kernel through ecall.  There is no MMU, so U-mode is kept out of
 * kernel memory by PMP (which, in the flat model, grants the whole address
 * space: see plat_init in arch/rv32/platform.c).
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
    la      sp, __kernel_stack_top
    .option pop

    /* Clear .bss (covers the kernel stack: it is .bss space). */
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
/* Trap frame layout shared with C (arch/rv32/regs.h):                  */
/*     0..124   x0..x31 (offset == 4 * register number)                 */
/*   128        mepc                                                    */
/*   132        mstatus                                                 */
/*   136        mcause                                                  */
/*   140        mtval                                                   */
/*   = 144 bytes, 16-byte aligned                                       */
/*                                                                      */
/* Two stacks meet here.  A trap from U-mode switches to the kernel      */
/* stack before pushing the frame; a trap from M-mode (timer interrupt   */
/* while a syscall runs, or a kernel bug) pushes onto whichever kernel   */
/* stack is already in use, which is what makes nesting safe.  A trap    */
/* clears mstatus.MIE in hardware, so the entry sequence itself cannot   */
/* be interrupted and may use t0 as scratch.                            */

    .equ FRAME_SIZE, 144

    .section .text.trap, "ax"
    .align 4
    .globl trap_entry
trap_entry:
    csrw    mscratch, t0            /* park t0: needed as scratch below */
    csrr    t0, mstatus
    srli    t0, t0, 11
    andi    t0, t0, 3               /* mstatus.MPP */
    beqz    t0, .Lfrom_user

    /* ---- trap from M-mode: stay on the current stack ---------------- */
    csrr    t0, mscratch            /* original t0 */
    addi    sp, sp, -FRAME_SIZE
    sw      t0, 20(sp)              /* x5 = t0 */
    addi    t0, sp, FRAME_SIZE
    sw      t0, 8(sp)               /* frame->sp = kernel sp at trap */
    j       .Lsave

.Lfrom_user:
    /* ---- trap from U-mode: switch to the kernel stack -------------- */
    csrr    t0, mscratch            /* original t0 */
    csrw    mscratch, sp            /* park the user sp */
    .option push
    .option norelax
    la      sp, __kernel_stack_top
    .option pop
    addi    sp, sp, -FRAME_SIZE
    sw      t0, 20(sp)              /* x5 = t0 */
    csrr    t0, mscratch
    sw      t0, 8(sp)               /* frame->sp = user sp */

.Lsave:
    sw      x1,    4(sp)
    sw      x3,   12(sp)            /* gp (unused, but keep the frame whole) */
    sw      x4,   16(sp)            /* tp */
    sw      x6,   24(sp)
    sw      x7,   28(sp)
    sw      x8,   32(sp)
    sw      x9,   36(sp)
    sw      x10,  40(sp)
    sw      x11,  44(sp)
    sw      x12,  48(sp)
    sw      x13,  52(sp)
    sw      x14,  56(sp)
    sw      x15,  60(sp)
    sw      x16,  64(sp)
    sw      x17,  68(sp)
    sw      x18,  72(sp)
    sw      x19,  76(sp)
    sw      x20,  80(sp)
    sw      x21,  84(sp)
    sw      x22,  88(sp)
    sw      x23,  92(sp)
    sw      x24,  96(sp)
    sw      x25, 100(sp)
    sw      x26, 104(sp)
    sw      x27, 108(sp)
    sw      x28, 112(sp)
    sw      x29, 116(sp)
    sw      x30, 120(sp)
    sw      x31, 124(sp)

    csrr    t0, mepc
    sw      t0, 128(sp)
    csrr    t0, mstatus
    sw      t0, 132(sp)
    csrr    t0, mcause
    sw      t0, 136(sp)
    csrr    t0, mtval
    sw      t0, 140(sp)

    mv      a0, sp
    call    trap_handler

    /* trap_handler returns the frame to resume on (exec and exit can
       replace it with a parent's saved frame). */
    mv      sp, a0
    j       .Lrestore

/* -------------------------------------------------------------------- */
/* Resume a frame: restore CSRs and registers, then mret.  a0 hands the  */
/* frame in (sp follows it).  Shared with plat_enter_user(), which is    */
/* how the very first user program is started.                          */

    .globl rv32_enter_user
rv32_enter_user:
    mv      sp, a0
    /* fall through */

.Lrestore:
    lw      t0, 128(sp)
    csrw    mepc, t0
    lw      t0, 132(sp)
    csrw    mstatus, t0

    /* Where does sp point after this?  Returning to U-mode restores the
       user stack the trap interrupted (frame->sp); returning to M-mode
       simply unwinds the frame. */
    srli    t1, t0, 11
    andi    t1, t1, 3
    bnez    t1, .Lkernel_sp
    j       .Lrestore_regs
.Lkernel_sp:
    addi    t0, sp, FRAME_SIZE
    sw      t0, 8(sp)

.Lrestore_regs:
    lw      x1,    4(sp)
    lw      x3,   12(sp)
    lw      x4,   16(sp)
    lw      x6,   24(sp)
    lw      x7,   28(sp)
    lw      x8,   32(sp)
    lw      x9,   36(sp)
    lw      x10,  40(sp)
    lw      x11,  44(sp)
    lw      x12,  48(sp)
    lw      x13,  52(sp)
    lw      x14,  56(sp)
    lw      x15,  60(sp)
    lw      x16,  64(sp)
    lw      x17,  68(sp)
    lw      x18,  72(sp)
    lw      x19,  76(sp)
    lw      x20,  80(sp)
    lw      x21,  84(sp)
    lw      x22,  88(sp)
    lw      x23,  92(sp)
    lw      x24,  96(sp)
    lw      x25, 100(sp)
    lw      x26, 104(sp)
    lw      x27, 108(sp)
    lw      x28, 112(sp)
    lw      x29, 116(sp)
    lw      x30, 120(sp)
    lw      x31, 124(sp)
    lw      x5,   20(sp)
    lw      sp,    8(sp)
    mret

/* -------------------------------------------------------------------- */

    .section .bss
    .balign 16
    .globl __kernel_stack
__kernel_stack:
    .space 32768
    .globl __kernel_stack_top
__kernel_stack_top:
