#ifndef _HARDWARE_RESETS_H
#define _HARDWARE_RESETS_H

/* TinyOS shim: pico-sdk's peripheral reset helpers.  dcd_init() resets the
 * USB controller before configuring it, which is also what makes a warm
 * reboot after a crash come up clean instead of inheriting a half-armed
 * endpoint from the previous run. */

#include "types.h"

#define RESETS_BASE           0x40020000u
#define RESETS_RESET_OFFSET   0x00000000u   /* write 1 = hold in reset       */
#define RESETS_RESET_DONE_OFFSET 0x00000008u /* 1 = reset finished          */

#define RESETS_RESET_USBCTRL_BITS 0x10000000u   /* bit 28                     */

void reset_block(uint32_t bits);
void unreset_block(uint32_t bits);
void unreset_block_wait(uint32_t bits);

#endif
