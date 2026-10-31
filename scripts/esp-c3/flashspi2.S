/* flashspi2.S - page program via the USR custom sequence.
 *
 * flashspi.S proved: FLASH_SE (hardware cmd bit 24) erases for real,
 * RDID works, RDSR result lives in RD_STATUS [15:0].  But FLASH_PP
 * (hardware cmd bit 25) writes nothing - the hardware cmd bits have no
 * data path, a data-carrying program must be a USR custom sequence:
 * USER/USER1/USER2 select cmd+addr+MOSI phases, W0 holds the data,
 * CMD.USR (bit 18) fires.
 *
 * Also re-verifies the littlefs partition at 0x100000 to prove the
 * earlier hardware-PP attempt did not scribble anywhere.
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
    li   a0, 'U'
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

    /* 1. littlefs partition intact? read 3 words at 0x100000 */
    li   a0, 0x100000
    la   a1, buf
    li   a2, 12
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    li   s1, 3
1:  lw   a0, 0(s0)
    call put_hex
    addi s0, s0, 4
    addi s1, s1, -1
    bnez s1, 1b

    /* 2. current content at 0x3FC000 (expect FFFFFFFF after last SE) */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 8
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex

    /* 3. USR page program: cmd 0x02, 24-bit addr, 32 data bits */
    li   t0, 0x60002000
    li   t1, (1 << 31) | (1 << 30) | (1 << 27)
    sw   t1, 0x18(t0)            /* USER: command+addr+MOSI */
    li   t1, (23 << 26)
    sw   t1, 0x1c(t0)            /* USER1: 24 addr bits (23 encoded) */
    li   t1, (7 << 28) | 0x02
    sw   t1, 0x20(t0)            /* USER2: 8-bit cmd, value 0x02 */
    li   t1, 0x11223344
    sw   t1, 0x58(t0)            /* W0: data */
    li   t1, 0x3FC000
    sw   t1, 0x04(t0)            /* ADDR */
    li   t1, 31                  /* 32 data bits - 1 */
    sw   t1, 0x24(t0)            /* MOSI_DLEN */
    /* WREN (hardware cmd bit 30) */
    li   t1, (1 << 30)
    sw   t1, 0x00(t0)
2:  lw   t2, 0x00(t0)
    slli t3, t2, 1
    bltz t3, 2b
    /* fire USR */
    li   t1, (1 << 18)
    sw   t1, 0x00(t0)
3:  lw   t2, 0x00(t0)
    slli t3, t2, 13             /* bit 18 -> MSB */
    bltz t3, 3b

    /* 4. read back: expect 11223344 */
    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 8
    li   t0, 0x40000130
    jalr t0
    call put_hex
    la   s0, buf
    lw   a0, 0(s0)
    call put_hex

    /* 5. clean up: WREN + hardware SE(0x3FC000) */
    li   t0, 0x60002000
    li   t1, 0x3FC000
    sw   t1, 0x04(t0)
    li   t1, (1 << 30)
    sw   t1, 0x00(t0)
4:  lw   t2, 0x00(t0)
    slli t3, t2, 1
    bltz t3, 4b
    li   t1, (1 << 24)
    sw   t1, 0x00(t0)
5:  lw   t2, 0x00(t0)
    slli t3, t2, 7
    bltz t3, 5b

    li   a0, 0x3FC000
    la   a1, buf
    li   a2, 8
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
6:  j    6b

    .section .data
    .balign 4
buf:    .space 32
