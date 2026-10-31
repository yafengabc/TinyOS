/* flashspi.S - drive the SPI1 flash controller (SPI_MEM) directly.
 *
 * All three ROM erase variants return 0 without erasing (pattern written
 * at 0x3FC000 survives resets and erases), so erase must go through the
 * SPI1 hardware command bits.  SPI1 is SPI_MEM #1 at 0x60002000
 * (REG_SPI_MEM_BASE(i) = DR_REG_SPI0_BASE - i*4096; DR_REG_SPI0_BASE =
 * 0x60003000).  Command bits in CMD_REG are R/W/SC: write 1 to fire,
 * the bit reads back 0 when done; WAITI makes the controller poll the
 * chip busy flag before finishing, so a cleared bit means the flash is
 * idle again.
 *
 *   CMD 0x00: RDID=28 RDSR=27 WRSR=26 PP=25 SE=24 BE=23 CE=22 WREN=30
 *   ADDR 0x04: address for SE/PP
 *   RD_STATUS 0x2C: SPI_MEM_STATUS [15:0] - RDSR result, WIP = bit0
 *   MOSI_DLEN 0x24: TX bits - 1 for PP
 *   W0 0x58: TX/RX FIFO word 0
 *
 * Read verification uses the proven ROM reader (0x40000130).
 */
    .macro CMD bit
    li   t0, 0x60002000
    li   t1, (1 << \bit)
    sw   t1, 0x00(t0)
1:  lw   t2, 0x00(t0)
    slli t3, t2, (31 - \bit)     /* bit -> MSB; andi can't hold 1<<24 */
    bltz t3, 1b
    /* report the state after this command */
    lw   a0, 0x2c(t0)            /* RD_STATUS */
    call put_hex
    lw   a0, 0x58(t0)            /* W0 */
    call put_hex
    .endm

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
    li   a0, 'P'
    call putc
    li   a0, '\r'
    call putc
    li   a0, '\n'
    call putc

    /* ROM chip record -> 4 MiB so the ROM reader can reach 0x3FC000 */
    li   a0, 0
    li   a1, 0x400000
    li   a2, 0x10000
    li   a3, 0x1000
    li   a4, 0x100
    li   a5, 0xFFFF
    li   t0, 0x40000134
    jalr t0
    call put_hex

    /* 1. RDID: controller path proof */
    CMD 28

    /* 2. RDSR baseline */
    CMD 27

    /* 3. WREN, then RDSR - WEL (bit1 of status) should now be set */
    CMD 30
    CMD 27

    /* 4. SE(0x3FC000) */
    li   t0, 0x60002000
    li   t1, 0x3FC000
    sw   t1, 0x04(t0)
    li   t1, (1 << 24)
    sw   t1, 0x00(t0)
2:  lw   t2, 0x00(t0)
    slli t3, t2, 7
    bltz t3, 2b
    lw   a0, 0x2c(t0)
    call put_hex

    /* 5. read back with the ROM reader: expect FFFFFFFF */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 16
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex

    /* 6. PP one word 0x11223344 at 0x3FC000 */
    li   t0, 0x60002000
    li   t1, 0x11223344
    sw   t1, 0x58(t0)            /* W0 */
    li   t1, 0x3FC000
    sw   t1, 0x04(t0)
    li   t1, 31                  /* 32 data bits - 1 */
    sw   t1, 0x24(t0)            /* MOSI_DLEN */
    li   t1, (1 << 30)           /* WREN first */
    sw   t1, 0x00(t0)
3:  lw   t2, 0x00(t0)
    slli t3, t2, 1
    bltz t3, 3b
    li   t1, (1 << 25)           /* FLASH_PP */
    sw   t1, 0x00(t0)
4:  lw   t2, 0x00(t0)
    slli t3, t2, 6
    bltz t3, 4b
    lw   a0, 0x2c(t0)
    call put_hex

    /* 7. read back: expect 11223344 */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 16
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex

    /* 8. SE again, read back: expect FFFFFFFF */
    li   t0, 0x60002000
    li   t1, 0x3FC000
    sw   t1, 0x04(t0)
    li   t1, (1 << 30)
    sw   t1, 0x00(t0)
5:  lw   t2, 0x00(t0)
    slli t3, t2, 1
    bltz t3, 5b
    li   t1, (1 << 24)
    sw   t1, 0x00(t0)
6:  lw   t2, 0x00(t0)
    slli t3, t2, 7
    bltz t3, 6b

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
7:  j    7b

    .section .data
    .balign 4
buf:    .space 32
