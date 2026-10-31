/* flashread512.S - does the ROM reader corrupt 512-byte reads?
 *
 * The kernel read block 0 of the littlefs partition (8x512 via
 * esp_rom_spiflash_read) and every segment matched the flashed image
 * EXCEPT the first one (offset 0..511) - while 32-byte reads at the same
 * address were fine.  Read 0x100000 with len=512 here, in a quiet
 * environment (no interrupts), and checksum it the same way the kernel
 * does (sum = sum*31 + byte, mod 2^32).  Expected from the host image:
 * 73aa896b.
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

    li   a0, 'R'
    call putc
    li   a0, '5'
    call putc
    li   a0, '\r'
    call putc
    li   a0, '\n'
    call putc

    /* ROM chip record -> 4 MiB */
    li   a0, 0
    li   a1, 0x400000
    li   a2, 0x10000
    li   a3, 0x1000
    li   a4, 0x100
    li   a5, 0xFFFF
    li   t0, 0x40000134
    jalr t0
    call put_hex

    /* one 512-byte read at 0x100000 */
    li   a0, 0x100000
    la   a1, buf
    li   a2, 512
    li   t0, 0x40000130
    jalr t0
    call put_hex

    /* checksum segments of 512 with the kernel's formula */
    la   s0, buf
    li   s1, 1
1:  li   s2, 0                   /* sum */
    li   s3, 512
2:  lbu  s4, 0(s0)
    li   t0, 31
    mul  s2, s2, t0
    add  s2, s2, s4
    addi s0, s0, 1
    addi s3, s3, -1
    bnez s3, 2b
    mv   a0, s2
    call put_hex
    addi s1, s1, -1
    bnez s1, 1b

    /* dump first 32 bytes for eyeballing */
    la   s0, buf
    li   s1, 8
3:  lw   a0, 0(s0)
    call put_hex
    addi s0, s0, 4
    addi s1, s1, -1
    bnez s1, 3b

    li   a0, 'O'
    call putc
    li   a0, 'K'
    call putc
    li   a0, '\r'
    call putc
    li   a0, '\n'
    call putc
4:  j    4b

    .section .data
    .balign 4
buf:    .space 1024
