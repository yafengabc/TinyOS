/* flashsz2.S - where did flashrw actually write, and what is at 0x100000?
 *
 * flashsz saw 0x100000 word0 change from f7ff0ff0 (littlefs superblock)
 * to 00000001 between runs.  If the chip is really 2 MiB (address pins
 * wrap), flashrw's writes at 0x3FC000 landed at 0x1FC000 physically.
 * Dump 16 bytes at 0x100000, 0x1FC000, 0x1FF000, and 0x3FC000-alias
 * 0x1FC000 already covered - plus 0x3FFFFC wrap check.
 */
    .section .text
    .globl _start
_start:
    la   sp, stack_scratch_end

    li   t0, 0x6001F000
    li   t1, 0x50D83AA1
    sw   t1, 0x64(t0)
    sw   x0, 0x48(t0)
    li   t0, 0x60008000
    li   t1, 0x50D83AA1
    sw   t1, 0xa8(t0)
    sw   x0, 0x90(t0)
    li   t1, 0x8F1D312A
    sw   t1, 0xb0(t0)
    li   t1, (1 << 30)
    sw   t1, 0xac(t0)

    li   a0, 'F'
    call putc
    li   a0, '2'
    call putc
    li   a0, '\r'
    call putc
    li   a0, '\n'
    call putc

    la   s2, addrs
    li   s3, 5
1:  lw   a0, 0(s2)
    la   a1, buf
    li   a2, 32
    li   t0, 0x40000130
    jalr t0
    mv   s1, a0
    mv   a0, s1
    call put_hex
    la   s0, buf
    li   s4, 8                   /* 8 words = 32 bytes */
2:  lw   a0, 0(s0)
    call put_hex
    addi s0, s0, 4
    addi s4, s4, -1
    bnez s4, 2b
    addi s2, s2, 4
    addi s3, s3, -1
    bnez s3, 1b

    li   a0, 'O'
    call putc
    li   a0, 'K'
    call putc
    li   a0, '\r'
    call putc
    li   a0, '\n'
    call putc
3:  j    3b

    .section .data
    .balign 4
addrs:  .word 0x100000, 0x1FC000, 0x1FF000, 0x1FFFFC, 0x3FC000
    .balign 4
buf:    .space 32
