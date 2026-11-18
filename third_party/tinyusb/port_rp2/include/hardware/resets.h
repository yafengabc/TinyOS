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
 * This macro is the RP2350 value; the RP2040 port is only reached through
 * reset_block()/unreset_block_wait() in drivers/usb/rp2040/tusb_shim.c, which
 * keep their own mask.  See docs/OPEN-ITEMS.md for the remaining 143 ms
 * timeout this leaves on the RP2040. */
#define RESETS_RESET_USBCTRL_BITS 0x10000000u   /* bit 28 (RP2350)            */

void reset_block(uint32_t bits);
void unreset_block(uint32_t bits);
void unreset_block_wait(uint32_t bits);

#endif
