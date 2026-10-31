/* TIMG0 -> INTMTX line 2 -> vectored slot 2: RESOLVED, works perfectly.
 *
 * FINAL SEQUENCE (all verified on the real C3, COM4):
 * - INTMTX: MAP[32]=2 (0x600C2000+0x80), PRI_2=1 (0x11C), THRESH=1 (0x194),
 *   ENABLE |= 1<<2 (0x104).  mcause bit31 | line number, so the handler
 *   must sit in SLOT 2, not slot 11 (MEIP).
 * - Vector table: mtvec is WARL-vectored on C3; base must be 256-aligned
 *   and every entry MUST be 4 bytes - wrap the table in .option norvc or
 *   `j` compresses to 2-byte c.j and base+4*cause lands mid-table.
 * - TIMG0: REGCLK (0xFC) needs 0xE0000000 (CLK_EN + TIMER/WDT_CLK_IS_ACTIVE;
 *   writing bit31 alone wipes the two default-1 bits and kills the timer).
 *   ALARMLO=10000, CONFIG = EN|INCREASE|AUTORELOAD|ALARM_EN|79<<13,
 *   INT_ENA=1 (0x70).  Readbacks are truthful (0x2710, 0xE009E400).
 * - Handler: INT_CLR (0x7C=1) + rewrite CONFIG bit10 (ALARM_EN self-clears
 *   on fire; AUTORELOAD does not restore it).  FULLY SILENT handler works.
 * - Handler MUST save/restore every register it touches: with ticks at
 *   100Hz, a bare handler WILL eventually land mid-putc in main and freeze
 *   it (t1/t2/t3 carnage).  MIE=0 during the handler, so sp is safe.
 *
 * THE GREAT RED HERRING (2026-10-31): for hours the tick chain looked like
 * it "required a UART FIFO write in the handler".  Reality: put_hex8 had
 * `mv a0, t5` (digit char) + `slli a0, 4` inside its loop, destroying the
 * value being printed - only the FIRST nibble was ever real.  Small counters
 * (entries < 0x10000000) therefore always printed 00000000, which read as
 * "zero interrupts".  Every "silent handler = no ticks" experiment, the
 * SYSTIMER fallback, and the whole bisection campaign chased this print bug.
 * The UART STATUS read, UPDATE kicks, REGCLK rewrites, GPIO writes - none
 * of it ever mattered. */

    .section .text
    .globl _start
_start:
    li   s0, 0x60000000       /* UART */

    la   t0, vectors
    ori  t0, t0, 1            /* vectored */
    csrw mtvec, t0

    /* watchdogs off: TG0WDT (ROM) + RTC WDT (esptool) */
    li   t0, 0x6001F000
    li   t1, 0x50D83AA1
    sw   t1, 0x64(t0)
    sw   zero, 0x48(t0)
    li   t0, 0x60008000
    li   t1, 0x50D83AA1
    sw   t1, 0xa8(t0)
    sw   zero, 0x90(t0)

    /* INTMTX: source 32 (TG0_T0) -> line 2 */
    li   t0, 0x600C2000
    li   t1, 2
    sw   t1, 0x80(t0)
    li   t1, 1
    sw   t1, 0x11c(t0)         /* PRI_2 */
    sw   t1, 0x194(t0)         /* THRESH */
    lw   t1, 0x104(t0)
    li   t2, 4
    or   t1, t1, t2
    sw   t1, 0x104(t0)         /* ENABLE |= (1<<2) */

    /* INTLEVEL = 0, MIE = 1 */
    csrr t1, mstatus
    li   t2, 0x7f
    not  t2, t2
    and  t1, t1, t2
    csrw mstatus, t1
    csrsi mstatus, 8

    li   a0, 'T'
    call putc
    li   a0, '\n'
    call putc

    /* EXPERIMENT G: map the load truncation across addresses.  Runs
     * BEFORE the TIMG init, so ZERO ticks can interfere.  Store
     * 0x12345678, load back: 0x10000000 = truncated (nibble mask),
     * 0x12345678 = working, 0x00000000 = undecoded/broken. */
    mv   a0, sp
    call put_hex8             /* where does the ROM leave sp? */
    li   a0, '\n'
    call putc
    li   t1, 0x12345678

    la   t0, entries
    sw   t1, 0(t0)
    lw   a0, 0(t0)             /* 1: 0x40380440 (entries) */
    call put_hex8
    li   a0, '\n'
    call putc

    li   t0, 0x40380C00
    sw   t1, 0(t0)
    lw   a0, 0(t0)             /* 2: 0x40380C00 (instr window, high) */
    call put_hex8
    li   a0, '\n'
    call putc

    li   t0, 0x3FC80C00
    sw   t1, 0(t0)
    lw   a0, 0(t0)             /* 3: 0x3FC80C00 (data window alias) */
    call put_hex8
    li   a0, '\n'
    call putc

    li   t0, 0x3FC8F000
    sw   t1, 0(t0)
    lw   a0, 0(t0)             /* 4: 0x3FC8F000 (data window, high) */
    call put_hex8
    li   a0, '\n'
    call putc

    addi t0, sp, -256
    sw   t1, 0(t0)
    lw   a0, 0(t0)             /* 5: sp-256 (below current stack) */
    call put_hex8
    li   a0, '\n'
    call putc

    /* TIMG0 forensics: write each field, read back immediately */
    li   t0, 0x6001F000
    li   t1, (1<<31)|(1<<30)|(1<<29)
    sw   t1, 0xfc(t0)          /* TIMG_CLK_EN (bit31, default 0: "registers
                               * can not be read or written by software")
                               * PLUS TIMER/WDT_CLK_IS_ACTIVE (defaults 1,
                               * preserve them - writing bit31 alone wipes
                               * the timer's functional clock, verified) */
    lw   a0, 0xfc(t0)          /* REGCLK readback, expect e0000000 */
    call put_hex8
    li   t1, 10000
    sw   t1, 0x10(t0)          /* ALARMLO */
    lw   a0, 0x10(t0)
    call put_hex8
    li   t1, 79 << 13
    sw   t1, 0x00(t0)          /* divider only */
    lw   a0, 0x00(t0)
    call put_hex8
    li   t1, (1<<31)|(1<<30)|(1<<29)|(79 << 13)
    sw   t1, 0x00(t0)          /* + EN|INCREASE|AUTORELOAD */
    lw   a0, 0x00(t0)
    call put_hex8
    li   t1, (1<<31)|(1<<30)|(1<<29)|(1<<10)|(79 << 13)
    sw   t1, 0x00(t0)          /* + ALARM_EN */
    lw   a0, 0x00(t0)
    call put_hex8
    sw   zero, 0x18(t0)
    sw   zero, 0x1c(t0)
    sw   zero, 0x20(t0)        /* T0LOAD */
    li   t1, 1
    sw   t1, 0x70(t0)          /* INT_ENA */

    /* UPDATE kick + first samples */
    li   t1, 0x80000000
    sw   t1, 0x0c(t0)
    lw   a0, 0x04(t0)
    call put_hex8
    lw   a0, 0x74(t0)
    call put_hex8
    lw   a0, 0x78(t0)
    call put_hex8
    li   a0, '\n'
    call putc

    /* rate measurement: dots vs x's reveal the real tick spacing.
     * Handler is SILENT; every ~512 dots the entries counter is printed
     * so silent ticks become visible. */
    la   s1, entries
    li   s3, 512
4:  li   a0, '.'
    call putc
    addi s3, s3, -1
    bnez s3, 4b
    li   s3, 512
    lw   a0, 0(s1)
    call put_hex8             /* entries since start */
    li   a0, '\r'
    call putc
    j    4b

/* ---- vectors: 32 x 4-byte slots, slot 2 = CPU line 2 ----------------- */
    .option push
    .option norvc
    .balign 256
    .globl vectors
vectors:
    j    sync
    j    sync
    j    tick                  /* line 2 */
    .rept 29
    j    sync
    .endr
    .option pop

sync:
    csrr a0, mcause
    li   a1, 'E'
    call putc
    mv   a1, a0                 /* working copy (same bug as old put_hex8) */
    li   a2, 8
1:  srli t5, a1, 28
    andi t5, t5, 0xf
    li   t6, 10
    bltu t5, t6, 2f
    addi t5, t5, 'a'-10
    j    3f
2:  addi t5, t5, '0'
3:  mv   a0, t5
    call putc
    slli a1, a1, 4
    addi a2, a2, -1
    bnez a2, 1b
    li   a0, '\n'
    call putc
4:  j    4b

tick:
    /* FULL register save/restore: a tick can land anywhere in main's
     * code (mid-putc, mid-init-check); clobbering t-regs there froze
     * the main loop and poisoned every measurement.  MIE=0 during the
     * handler so no nesting - sp use is safe. */
    addi sp, sp, -40
    sw   t0, 0(sp)
    sw   t1, 4(sp)
    sw   t2, 8(sp)
    sw   t3, 12(sp)
    sw   t4, 16(sp)
    sw   t6, 20(sp)
    la   t2, entries          /* FIRST action: prove the handler ran */
    lw   t3, 0(t2)
    addi t3, t3, 1
    sw   t3, 0(t2)
    li   t0, 0x6001F000
    li   t6, 1
    sw   t6, 0x7c(t0)          /* INT_CLR */
    li   t6, (1<<31)|(1<<30)|(1<<29)|(1<<10)|(79 << 13)
    sw   t6, 0x00(t0)          /* re-arm ALARM_EN (self-clears on fire) */
    /* FULLY SILENT: no UART write at all.  With put_hex8 fixed, the
     * snapshots alone prove whether ticks arrive - the "handler must
     * write the UART FIFO" rule was an artifact of the print bug. */
    lw   t0, 0(sp)
    lw   t1, 4(sp)
    lw   t2, 8(sp)
    lw   t3, 12(sp)
    lw   t4, 16(sp)
    lw   t6, 20(sp)
    addi sp, sp, 40
    mret

    .section .data
    .balign 4
ticks:
    .word 0
entries:
    .word 0

/* ---- put_hex8(a0) ----------------------------------------------------
 * BUG FIXED: the old loop did `mv a0, t5` (digit char) then `slli a0,4`,
 * destroying the value being printed - only the TOP nibble was ever real.
 * Every hex readback this session (entries snapshots, TIMG readbacks, the
 * "silent handler = zero interrupts" conclusion) was poisoned by this.
 * The working copy now lives in a1. */
put_hex8:
    addi sp, sp, -16
    sw   ra, 12(sp)
    mv   a1, a0                 /* working copy */
    li   a2, 8
1:  srli t5, a1, 28
    andi t5, t5, 0xf
    li   t6, 10
    bltu t5, t6, 2f
    addi t5, t5, 'a' - 10
    j    3f
2:  addi t5, t5, '0'
3:  mv   a0, t5
    call putc
    slli a1, a1, 4
    addi a2, a2, -1
    bnez a2, 1b
    lw   ra, 12(sp)
    addi sp, sp, 16
    ret

/* ---- putc with drain delay ------------------------------------------- */
putc:
    li   t1, 0x6000001c
1:  lw   t2, 0(t1)
    srli t2, t2, 16
    andi t2, t2, 0xff
    li   t3, 100
    bgeu t2, t3, 1b
    li   t1, 0x60000000
    sw   a0, 0(t1)
    li   t3, 2000
2:  addi t3, t3, -1
    bnez t3, 2b
    ret

    .balign 4
