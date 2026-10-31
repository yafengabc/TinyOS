/* Which CSR instructions does the C3 actually accept?
 *
 * Prints one character per completed operation; the first missing
 * character marks the illegal instruction.  Runs in M-mode with a
 * 1 GiB NAPOT PMP, exactly like the umode probe. */

    .section .text
    .globl _start
_start:
    /* PMP: one 1 GiB NAPOT covering code + UART, or U-mode sees nothing. */
    li   t0, 0x40000000
    csrw pmpaddr0, t0
    li   t0, 0x1f
    csrw pmpcfg0, t0
    fence

    li   s0, 0x60000000       /* UART FIFO (t1 is reserved: putc clobbers it) */

    /* 1: csrw mie, zero -- the instruction our start.S dies on */
    csrw mie, zero
    li   a0, '1'
    call putc

    /* 2: read mie back */
    csrr t2, mie
    li   a0, '2'
    call putc

    /* 3: csrw mie, t0 with t0 = 0 (what esp-idf does instead) */
    li   t2, 0
    csrw mie, t2
    li   a0, '3'
    call putc

    /* 4: csrsi mstatus, 8 */
    csrsi mstatus, 8
    li   a0, '4'
    call putc

    /* 5: csrci mstatus, 8 */
    csrci mstatus, 8
    li   a0, '5'
    call putc

    /* 6: enable MEIE (bit 11) via csrs mie -- what plat_timer_init does */
    li   t2, 0x800
    csrs mie, t2
    li   a0, '6'
    call putc

    /* 7: read mtval (the ROM dump reads it fine, we should too) */
    csrr t2, mtval
    li   a0, '7'
    call putc

    /* all passed: newline and spin forever */
    li   a0, '\n'
    call putc
1:  j    1b

/* ---- putc: same as probe_common.S, s0 kept free --------------------- */
putc:
    li   t1, 0x6000001c
1:  lw   t2, 0(t1)
    srli t2, t2, 16
    andi t2, t2, 0xff
    li   t3, 100
    bgeu t2, t3, 1b
    li   t1, 0x60000000
    sw   a0, 0(t1)
    /* drain: the ROM's banner text is still in flight ahead of us */
    li   t3, 2000
2:  addi t3, t3, -1
    bnez t3, 2b
    ret

    .balign 4
