/* flasher3.S - last ROM variant: SPI_sector_erase (old API, 0x4000020c).
 *
 * Both esp_rom_spiflash_erase_sector and the high-level
 * spi_flash_erase_sector return 0 without erasing.  The old-style
 * SPI_sector_erase takes a byte address on the classic ESP32.
 * Also probes RDID-style info: esp_rom_spiflash_read_statushigh and
 * spi_flash_get_chip_size to see the ROM's current record.
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
    li   a0, '3'
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

    /* spi_flash_get_chip_size(): see what the ROM believes */
    li   t0, 0x40000168
    jalr t0
    call put_hex

    /* read 0x3FC000: expect the old pattern 11223344 */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 16
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex

    /* SPI_sector_erase(0x3FC000) - old API, byte address */
    li   a0, 0x3FC000
    li   t0, 0x4000020c
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
