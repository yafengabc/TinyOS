/* flasher2.S - the high-level ROM flash API: unlock + erase_sector.
 *
 * esp_rom_spiflash_erase_sector (0x40000128) returns 0 but provably does
 * not erase (the written pattern survives across resets).  Try the
 * higher-level entry points: spi_flash_unlock clears the status-register
 * protection bits, spi_flash_erase_sector is the guarded high-level call.
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
    li   a0, 'E'
    call putc
    li   a0, '\r'
    call putc
    li   a0, '\n'
    call putc

    /* chip record: 4 MiB */
    li   a0, 0
    li   a1, 0x400000
    li   a2, 0x10000
    li   a3, 0x1000
    li   a4, 0x100
    li   a5, 0xFFFF
    li   t0, 0x40000134
    jalr t0
    call put_hex

    /* read current content of 0x3FC000 */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 16
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex

    /* spi_flash_unlock(): no args */
    li   t0, 0x40000180
    jalr t0
    call put_hex

    /* spi_flash_erase_sector(0x3FF) - high level */
    li   a0, 0x3FF
    li   t0, 0x40000188
    jalr t0
    call put_hex

    /* read back */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 16
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex

    /* read the status register via esp_rom_spiflash_read_status */
    li   t0, 0x40000158
    la   a0, buf
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
