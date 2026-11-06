#ifndef _PICO_FIX_RP2040_USB_DEVICE_ENUMERATION_H
#define _PICO_FIX_RP2040_USB_DEVICE_ENUMERATION_H

/* TinyOS shim: pico-sdk ships a workaround for RP2040 erratum RP2040-E15
 * (a device that has just been attached can be missed by some hosts because
 * the D+ pull-up comes up too early).  The workaround is only compiled in
 * when TUD_OPT_RP2040_USB_DEVICE_ENUMERATION_FIX is set, which this port
 * leaves off: the target is an RP2350, and the fix itself is a timing hack
 * around the RP2040's PHY.  The declaration exists so the header can be
 * included unconditionally. */

void rp2040_usb_device_enumeration_fix(void);

#endif
