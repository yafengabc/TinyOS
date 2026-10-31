/* flashrw.S - verify the ROM flash write/erase path on a safe sector.
 *
 * Works at 0x3FC000 (4 MiB flash, 16 KiB from the end): far from the
 * kernel image at 0x0 and the littlefs partition at 0x100000.
 *
 * Tests esp_rom_spiflash_erase_sector's argument convention - sector
 * NUMBER (0x3FC000 >> 12 = 0x3FF) is tried first; if the sector does not
 * read back as erased after the call, the alternative (byte address) is
 * what the ROM wants and the caller in board/esp32c3/platform.c must be
 * fixed.  Then writes a pattern and reads it back, then erases again.
 */
    .section .text
    .globl _start
_start:
    la   sp, stack_scratch_end

    /* watchdogs off, as everywhere */
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
    li   a0, 'W'
    call putc
    li   a0, '\r'
    call putc
    li   a0, '\n'
    call putc

    /* 1. read the target before touching it (expect FFFFFFFF...) */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 32
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex

    /* 2. erase sector NUMBER 0x3FF (byte addr / 4096) */
    li   a0, 0x3FF
    li   t0, 0x40000128
    jalr t0
    call put_hex

    /* 3. read back: word 0 must now be FFFFFFFF if NUMBER is right */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 32
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex

    /* 4. write two pattern words */
    la   a1, pat
    li   a0, 0x3FC000
    li   a2, 8
    li   t0, 0x4000012c
    jalr t0
    call put_hex

    /* 5. read back the pattern */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 32
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex
    lw   a0, 4(s0)
    call put_hex

    /* 6. erase again, read back: both words must be FFFFFFFF */
    li   a0, 0x3FF
    li   t0, 0x40000128
    jalr t0
    call put_hex
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 32
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
buf:    .space 64
    .balign 4
pat:    .word 0x11223344, 0x55667788
