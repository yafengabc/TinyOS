/* Can we read SPI flash through the data window at 0x3C000000?
 *
 * The ROM maps flash for its own use (it is executing from there); the
 * question is what guest code sees at the data alias, and whether the
 * content at 0x3C100000 matches the ramdisk image we burned there.
 */

    .section .text
    .globl _start
_start:
    li   s0, 0x60000000

    /* 'W' = alive, then hex-dump 16 bytes from three addresses. */
    li   a0, 'W'
    call putc
    li   a0, '\n'
    call putc

    li   a0, 'A'                 /* window base 0x3C000000 */
    call putc
    li   a0, ' '
    call putc
    li   a1, 0x3C000000
    call dump16

    li   a0, 'B'                 /* ramdisk partition 0x3C100000 */
    call putc
    li   a0, ' '
    call putc
    li   a1, 0x3C100000
    call dump16

    li   a0, 'C'                 /* instruction alias of SRAM top */
    call putc
    li   a0, ' '
    call putc
    li   a1, 0x3CDC0000
    call dump16

    li   a0, '\n'
    call putc
1:  j    1b

/* dump16(a1): 16 bytes as "xx " */
dump16:
    addi sp, sp, -16
    sw   ra, 12(sp)
    mv   a2, a1
    li   a3, 16
1:  lbu  a0, 0(a2)
    call put_hex8
    li   a0, ' '
    call putc
    addi a2, a2, 1
    addi a3, a3, -1
    bnez a3, 1b
    li   a0, '\n'
    call putc
    lw   ra, 12(sp)
    addi sp, sp, 16
    ret

/* put_hex8(a0): two hex digits */
put_hex8:
    srli t5, a0, 4
    andi t5, t5, 0xf
    call nib
    andi t5, a0, 0xf
    call nib
    ret
nib:
    li   t6, 10
    bltu t5, t6, 1f
    addi t5, t5, 'a' - 10
    j    2f
1:  addi t5, t5, '0'
2:  mv   a0, t5
    j    putc

/* putc: writes, then burns time so the byte leaves before any reset. */
putc:
    li   t1, 0x6000001c
1:  lw   t2, 0(t1)
    srli t2, t2, 16
    andi t2, t2, 0xff
    li   t3, 100
    bgeu t2, t3, 1b
    li   t1, 0x60000000
    sw   a0, 0(t1)
    li   t3, 2000
2:  addi t3, t3, -1
    bnez t3, 2b
    ret

    .balign 4
