/* Shared helpers for the ESP32-C3 probes.
 *
 * Everything here writes UART0 directly.  Three rules came out of the bus
 * experiments and are baked in:
 *
 *   - A store into the UART FIFO is not a byte on the wire.  Anyone who resets
 *     before the FIFO drains loses the byte silently, which looks exactly like
 *     "the code never ran".
 *   - The ROM leaves up to 70 characters queued before jumping to the entry
 *     point, so a probe that starts writing immediately races that text.
 *   - The FIFO is 128 bytes deep, so a long report must wait for room.
 */
    .option norvc

    .section .text

/* Wait until the FIFO is nearly empty.  Watches TXFIFO_CNT (STATUS[23:16]). */
    .globl wait_drain
wait_drain:
    li   t1, 0x6000001c
    li   t3, 8
1:  lw   t2, 0(t1)
    srli t2, t2, 16
    andi t2, t2, 0xff
    bltu t2, t3, 2f
    j    1b
2:  ret

/* putc(a0): one byte out, waiting for room in the FIFO. */
    .globl putc
putc:
    li   t1, 0x6000001c
1:  lw   t2, 0(t1)
    srli t2, t2, 16
    andi t2, t2, 0xff
    li   t3, 100
    bgeu t2, t3, 1b
    li   t1, 0x60000000
    sw   a0, 0(t1)
    ret

/* puts(a0 = ptr, a1 = len) */
    .globl puts
puts:
    beqz a1, 2f
    addi sp, sp, -16
    sw   ra, 12(sp)
    sw   s0, 8(sp)
    sw   s1, 4(sp)
    mv   s0, a0
    mv   s1, a1
1:  lbu  a0, 0(s0)
    call putc
    addi s0, s0, 1
    addi s1, s1, -1
    bnez s1, 1b
    lw   s1, 4(sp)
    lw   s0, 8(sp)
    lw   ra, 12(sp)
    addi sp, sp, 16
2:  ret

/* put_hex(a0): eight lowercase digits then CRLF. */
    .globl put_hex
put_hex:
    addi sp, sp, -32
    sw   ra, 28(sp)
    sw   s0, 24(sp)
    sw   s1, 20(sp)
    sw   s2, 16(sp)
    mv   s0, a0
    li   s1, 8
1:  srli s2, s0, 28
    andi s2, s2, 0xf
    li   t3, 10
    bltu s2, t3, 2f
    addi s2, s2, 'a' - 10
    j    3f
2:  addi s2, s2, '0'
3:  mv   a0, s2
    call putc
    slli s0, s0, 4
    addi s1, s1, -1
    bnez s1, 1b
    li   a0, '\r'
    call putc
    li   a0, '\n'
    call putc
    lw   s2, 16(sp)
    lw   s1, 20(sp)
    lw   s0, 24(sp)
    lw   ra, 28(sp)
    addi sp, sp, 32
    ret

/* putc_im(imm) is a macro rather than a function so probes can emit single
   marker characters without setting up a0 first. */
    .macro PUTC_IMM ch
    li   a0, \ch
    call putc
    .endm

/* The ROM hands over no stack.  1 KiB is plenty for these helpers. */
    .section .data
    .balign 4
    .globl stack_scratch_end
stack_scratch:
    .space 1024
stack_scratch_end:
    .balign 4
