/* flashread.S - probe the ROM SPI flash reader from a guest image.
 *
 * The kernel cannot serve littlefs from SPI flash yet (plat_block_read
 * returns -1) because the cache window at 0x3C000000 does not answer guest
 * reads (scripts/esp-c3/drom.S).  The pragmatic path is the ROM's own
 * reader, which the second-stage bootloader has already used to probe the
 * chip and load this very image:
 *
 *   esp_rom_spiflash_read(uint32_t src_addr, uint32_t *dest, int32_t len)
 *   esp_rom_spiflash_write(uint32_t dest, const uint32_t *src, int32_t len)
 *   esp_rom_spiflash_erase_sector(uint32_t sector)          (4 KiB units)
 *   -> 0x40000130 / 0x4000012c / 0x40000128, 0 == success
 *
 * Expected results on the real board:
 *   read 0x0       -> 0xE9 image magic, then segment count / entry
 *   read 0x100000  -> 0xFF... (erased) unless build/ramdisk.bin is burned
 */
    .section .text
    .globl _start
_start:
    la   sp, stack_scratch_end

    /* Watchdogs off (TG0WDT, RTC WDT, super WDT) - same sequence the
     * kernel uses, see tick.S. */
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
    li   a0, 'R'
    call putc
    li   a0, '\r'
    call putc
    li   a0, '\n'
    call putc

    /* --- read #1: image header at flash 0x0 -------------------------- */
    li   a0, 0
    la   a1, buf1
    li   a2, 64
    li   t0, 0x40000130
    jalr t0                       /* a0 = result, 0 == OK */
    call put_hex
    la   s0, buf1
    li   s1, 4
1:  lw   a0, 0(s0)                /* words 0..3 of the image header */
    call put_hex
    addi s0, s0, 4
    addi s1, s1, -1
    bnez s1, 1b

    /* --- read #2: the future littlefs partition at 0x100000 ---------- */
    li   a0, 0x100000 >> 0        /* 1 MiB in */
    la   a1, buf2
    li   a2, 64
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf2
    li   s1, 4
2:  lw   a0, 0(s0)                /* 0xFFFFFFFF when erased */
    call put_hex
    addi s0, s0, 4
    addi s1, s1, -1
    bnez s1, 2b

    /* --- read #3: a mid-image word (the banner string is in seg 0) --- */
    li   a0, 0x40
    la   a1, buf1
    li   a2, 32
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf1
    li   s1, 2
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
buf1:   .space 128
    .balign 4
buf2:   .space 128
