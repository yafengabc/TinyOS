/*
 * RP2350 XIP setup ("boot2"), run once from SRAM before the kernel starts
 * using flash at 150 MHz.
 *
 * Why this exists: the RP2350 boot ROM leaves QMI in a conservative mode
 * (03h serial read, CLKDIV=12) whose read sampling delay is counted in
 * clk_sys cycles.  That is safe at the ROM's boot clock, but once
 * pll_sys_init() moves clk_sys to 150 MHz the same delay is 12.5x too
 * short for the flash to drive the bus, and the first instruction fetched
 * after the switch can be sampled before the data is valid.  MicroPython
 * builds its RP2350 port with PICO_EMBED_XIP_SETUP=1 for exactly this
 * reason ("to put flash into continuous read mode").
 *
 * This is the ARM half of pico-sdk's src/rp2350/boot_stage2/boot2_w25q080.S
 * (BSD-3-Clause, Copyright (c) 2019-2022 Raspberry Pi (Trading) Ltd.),
 * reduced to the QMI timing + continuous-read setup.  On RP2350 boot2 is a
 * normal function: it configures QMI and returns; the ROM does not run it.
 * It must execute from SRAM because it reconfigures the interface the
 * instruction fetches come from.
 *
 * Constants are from the RP2350 register headers:
 *   PADS_QSPI_BASE 0x40040000, XIP_QMI_BASE 0x400d0000,
 *   alias CLR offset 0x3000.
 */

#ifdef TOS_RP2350_BOOT2

    .syntax unified
    .thumb

    .section .text.boot2, "ax"
    .p2align 2
    .globl  __boot2_start
    .globl  __boot2_end
    .thumb_func
__boot2_start:
    push    {lr}

    /* SCLK pad: drive strength 2, slew fast. */
    ldr     r3, =0x40040000
    movs    r0, #0x21
    str     r0, [r3, #0x04]

    /* SD0..SD3: input Schmitt trigger off (faster edges, like the SDK). */
    ldr     r3, =0x40043000             /* PADS_QSPI_BASE + CLR alias */
    movs    r0, #0x02                   /* SCHMITT bit */
    str     r0, [r3, #0x08]
    str     r0, [r3, #0x0c]
    str     r0, [r3, #0x10]
    str     r0, [r3, #0x14]

    /* QMI: CLKDIV=2 (75 MHz at 150 MHz clk_sys), RXDELAY=2, cooldown=1. */
    ldr     r3, =0x400d0000
    ldr     r0, =(1 << 30) | (2 << 8) | (2 << 0)
    str     r0, [r3, #0x0c]             /* M0_TIMING */

    /* Read command EBh with mode bits A0h: continuous quad read. */
    ldr     r0, =(0xeb << 0) | (0xa0 << 8)
    str     r0, [r3, #0x14]             /* M0_RCMD */

    /* Serial prefix, quad address/mode/dummy/data, 8-bit prefix/suffix. */
    ldr     r0, =(2 << 2) | (2 << 4) | (2 << 6) | (2 << 8) | \
                (1 << 12) | (2 << 14) | (4 << 16)
    str     r0, [r3, #0x10]             /* M0_RFMT */

    /* One dummy read latches the mode bits into the flash. */
    ldr     r1, =0x14000000
    ldrb    r1, [r1]

    /* From now on the flash expects no instruction prefix. */
    bic     r0, r0, #(3 << 12)
    str     r0, [r3, #0x10]

    dsb
    isb
    pop     {pc}
__boot2_end:
    .ltorg

#endif  /* TOS_RP2350_BOOT2 */
