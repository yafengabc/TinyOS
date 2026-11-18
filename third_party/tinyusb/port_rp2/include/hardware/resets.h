#ifndef _HARDWARE_RESETS_H
#define _HARDWARE_RESETS_H

/* TinyOS shim: pico-sdk's peripheral reset helpers.  dcd_init() resets the
 * USB controller before configuring it, which is also what makes a warm
 * reboot after a crash come up clean instead of inheriting a half-armed
 * endpoint from the previous run. */

#include "types.h"

/* RESETS_BASE is NOT defined here: it is per chip and comes from the board's
 * memmap (RP2040 0x4000c000, RP2350 0x40020000).  This header used to hardcode
 * the RP2350 value for both ports, so on the RP2040 every reset_block() call
 * wrote an unmapped address: dcd_init() never reset the controller, and
 * unreset_block_wait() polled a RESET_DONE that always read 0 - 1 000 000
 * iterations of an unmapped APB read, which measured 6.1 s on the board and
 * was the real cause of the "boot takes ten seconds" report. */
#define RESETS_RESET_OFFSET   0x00000000u   /* write 1 = hold in reset       */
#define RESETS_RESET_DONE_OFFSET 0x00000008u /* 1 = reset finished          */

/* USBCTRL's reset bit differs per chip: 24 on the RP2040, 28 on the RP2350.
 * This used to be the RP2350 value for both, so on the RP2040 dcd_init()
 * reset a block that does not exist and then polled a RESET_DONE bit that
 * never sets: 1 000 000 iterations, measured at 6.0 s of real time on the
 * board (the uart timestamp capture in docs/acceptance).  TOS_ARM_M0P is the
 * RP2040 arch define, and it reaches every translation unit including this
 * one; the board memmaps carry the same value as RESETS_USBCTRL. */
#ifdef TOS_ARM_M0P
#define RESETS_RESET_USBCTRL_BITS 0x01000000u   /* bit 24 (RP2040)            */
#else
#define RESETS_RESET_USBCTRL_BITS 0x10000000u   /* bit 28 (RP2350)            */
#endif

void reset_block(uint32_t bits);
void unreset_block(uint32_t bits);
void unreset_block_wait(uint32_t bits);

#endif
