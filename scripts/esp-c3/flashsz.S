/* flashsz.S - find the flash address ceiling of esp_rom_spiflash_read.
 *
 * flashrw.S showed reads failing at 0x3FC000 while flashread.S succeeded
 * at 0x0..0x100000.  Binary-search the boundary: read 6 addresses, print
 * result + first word for each.  A FAIL result (1) with an unchanged
 * buffer marks the first address above g_rom_spiflash_chip.chip_size.
 */
    .section .text
    .globl _start
_start:
    la   sp, stack_scratch_end

    /* watchdogs off */
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
    li   a0, 'S'
    call putc
    li   a0, '\r'
    call putc
    li   a0, '\n'
    call putc

    la   s2, addrs
    li   s3, 6
1:  lw   a0, 0(s2)
    la   a1, buf
    li   a2, 16
    li   t0, 0x40000130
    jalr t0
    mv   s1, a0                  /* keep result; put_hex clobbers a0 */
    mv   a0, s1
    call put_hex
    la   a0, buf
    lw   a0, 0(a0)               /* first word read (0 if read failed) */
    call put_hex
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
2:  j    2b

    .section .data
    .balign 4
addrs:  .word 0x000000, 0x0FFFFC, 0x100000, 0x1FFFFC, 0x200000, 0x3FC000
    .balign 4
buf:    .space 32
