/*
 * RP2350 crystal oscillator: the chip-level startup sequence.
 *
 * This is shared by the ARM and RISC-V ports (board/rp2350-generic and
 * board/rp2350rv-generic): XOSC is the same block on both, and its startup
 * rules are a fact about the silicon, not about the ISA.  Keeping one copy
 * here is what stops the two platform.c files from drifting apart.
 *
 * pico-sdk's xosc_init() programs STARTUP.DELAY *before* enabling, so
 * XOSC_STATUS.STABLE is only asserted after the crystal has had its startup
 * time.  The delay counts 256 crystal periods:
 *
 *     (((XOSC_HZ / KHZ) + 128) / 256) * 6
 *     = ((12000 + 128) / 256) * 6 = 47 * 6 = 282   (~6 ms at 12 MHz)
 *
 * The reset value is 0, i.e. STABLE asserts as soon as the amplitude crosses
 * its threshold while the frequency is still drifting.  Moving clk_ref (and
 * with it clk_sys) onto such a crystal changes the clock in the middle of a
 * flash read, and the board dies before its first byte: measured on the
 * Pico 2 as roughly half of the cold boots failing, sometimes with a load
 * access fault inside the first console line (issue #7 follow-up).
 *
 * The two cases are not symmetric:
 *   - this code enables it (ENABLE was off, the cold-boot case): the
 *     hardware delay above is the whole wait;
 *   - the ROM enabled it earlier with DELAY = 0 (warm/UF2 boot): that cannot
 *     be rewritten for the current start, so wait a full startup period
 *     after STABLE instead.
 *
 * Include after platform.h and the board's memmap: it uses mmio_read32/
 * mmio_write32 and plat_pre_tick_spins().
 */
#ifndef TINYOS_RP2350_XOSC_H
#define TINYOS_RP2350_XOSC_H

#define XOSC_BASE               0x40048000u
#define XOSC_CTRL               (XOSC_BASE + 0x00u)
#define XOSC_STATUS             (XOSC_BASE + 0x04u)
#define XOSC_STARTUP            (XOSC_BASE + 0x0cu)
#define XOSC_CTRL_ENABLE_12M    ((0xfabu << 12) | 0xaa0u)
#define XOSC_STATUS_STABLE      (1u << 31)
#define XOSC_STATUS_ENABLED     (1u << 12)
/* 282 is pico-sdk's default (multiplier 6).  This board's crystal is slow
 * to settle - 6 ms still left roughly one cold boot in ten dead - so the
 * multiplier is raised to 20: 47 * 20 = 940 counts of 256 periods each,
 * about 20 ms at 12 MHz.  The SDK documents the multiplier as the knob
 * for exactly this ("to accommodate slow-starting oscillators"). */
#define XOSC_STARTUP_DELAY_12M  940u

/* Bounded: callers continue on the ROM's clock if the crystal never starts. */
static inline int rp2350_xosc_wait_stable(void)
{
    for (uint32_t i = 0; i < plat_pre_tick_spins(1000000u); i++) {
        if (mmio_read32(XOSC_STATUS) & XOSC_STATUS_STABLE)
            return 1;
    }
    return 0;
}

/* Returns 1 when clk_ref may be moved onto the crystal. */
static inline int rp2350_xosc_start(void)
{
    uint32_t st = mmio_read32(XOSC_STATUS);

    if (!(st & XOSC_STATUS_ENABLED)) {
        mmio_write32(XOSC_STARTUP, XOSC_STARTUP_DELAY_12M);
        mmio_write32(XOSC_CTRL, XOSC_CTRL_ENABLE_12M);
        return rp2350_xosc_wait_stable();
    }
    if (!(st & XOSC_STATUS_STABLE) && !rp2350_xosc_wait_stable())
        return 0;
    /* ROM started it with DELAY = 0; give it the period it never waited
     * (same order as the hardware delay above). */
    for (volatile uint32_t d = 0; d < plat_pre_tick_spins(660000u); d++)
        ;
    return 1;
}

#endif /* TINYOS_RP2350_XOSC_H */
