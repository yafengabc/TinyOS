/* Per-chip USBCTRL reset bit, for the vendored TinyUSB RP2 port.
 *
 * The RP2350 keeps USBCTRL at bit 28 (the RP2040 has it at 24).  See the
 * RP2040 copy of this header for why it lives outside third_party.
 */
#ifndef TOS_USB_RESETS_H
#define TOS_USB_RESETS_H

#define TOS_RESETS_USBCTRL 0x10000000u   /* RP2350: bit 28 */

#endif
