/* flashcfg.S - fix g_rom_spiflash_chip.chip_size via
 * esp_rom_spiflash_config_param, then run the full erase/write/read
 * cycle on the safe sector at 0x3FC000.
 *
 * flashsz/flashsz2 proved ROM ops are range-checked against chip_size
 * (currently 2 MiB).  config_param(deviceId, chip_size, block_size,
 * sector_size, page_size, status_mask) updates the ROM's chip record.
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
    li   a0, 'C'
    call putc
    li   a0, '\r'
    call putc
    li   a0, '\n'
    call putc

    /* 1. read 0x3FC000 before: expect FAIL (1) - beyond 2 MiB record */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 16
    li   t0, 0x40000130
    jalr t0
    call put_hex

    /* 2. reconfigure: 4 MiB chip, 64 KiB blocks, 4 KiB sectors,
     *    256 B pages, single-command status mask */
    li   a0, 0                    /* deviceId: unused here */
    li   a1, 0x400000             /* chip_size */
    li   a2, 0x10000              /* block_size */
    li   a3, 0x1000               /* sector_size */
    li   a4, 0x100                /* page_size */
    li   a5, 0xFFFF               /* status_mask */
    li   t0, 0x40000134
    jalr t0
    call put_hex

    /* 3. read again: expect OK (0), word0 = FFFFFFFF */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 16
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex

    /* 4. erase sector 0x3FF */
    li   a0, 0x3FF
    li   t0, 0x40000128
    jalr t0
    call put_hex

    /* 5. read back: FFFFFFFF */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 16
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex

    /* 6. write the pattern */
    la   a1, pat
    li   a0, 0x3FC000
    li   a2, 8
    li   t0, 0x4000012c
    jalr t0
    call put_hex

    /* 7. read back: 11223344 55667788 */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 16
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex
    lw   a0, 4(s0)
    call put_hex

    /* 8. erase again, read back: FFFFFFFF */
    li   a0, 0x3FF
    li   t0, 0x40000128
    jalr t0
    call put_hex
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 16
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex

    li   a0, 'O'
    call putc
    li   a0, 'K'
    call putc
    li   a0, '\r'
    call putc
    li   a0, '\n'
    call putc
1:  j    1b

    .section .data
    .balign 4
buf:    .space 32
    .balign 4
pat:    .word 0x11223344, 0x55667788
