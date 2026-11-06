#ifndef _HARDWARE_ADDRESS_MAPPED_H
#define _HARDWARE_ADDRESS_MAPPED_H

/* TinyOS shim: the small slice of pico-sdk's hardware/address_mapped.h that
 * TinyUSB's RP2040 port needs.  pico-sdk wraps every peripheral register in
 * a typedef plus an atomic set/clear alias; on the RP2350 the aliases live
 * 0x2000 (set) and 0x3000 (clear) above the register block, which is what
 * dcd_rp2040.c means by usb_hw_set / usb_hw_clear. */

#include "pico/types.h"

typedef volatile uint32_t io_rw_32;
typedef volatile uint32_t io_ro_32;
typedef volatile uint32_t io_wo_32;

#ifndef _u
#define _u(x) x##u
#endif

#ifndef __unused
#define __unused __attribute__((unused))
#endif

/* Atomic register aliases.  Writing the alias sets or clears the bits of the
 * underlying register without a read-modify-write, so an interrupt handler
 * cannot lose an update made by the main loop. */
#define hw_set_alias_untyped(addr)   ((void *)((uintptr_t)(addr) + 0x2000u))
#define hw_clear_alias_untyped(addr) ((void *)((uintptr_t)(addr) + 0x3000u))

#endif
