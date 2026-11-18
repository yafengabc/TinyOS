/* Per-chip USBCTRL reset bit, for the vendored TinyUSB RP2 port.
 *
 * TinyUSB's dcd_rp2040.c resets the controller through reset_block() and
 * waits for RESET_DONE; the bit number differs between chips (24 on the
 * RP2040, 28 on the RP2350) and the vendored header used to carry the RP2350
 * value for both - which cost the RP2040 6 s per boot (commit 998efe5).
 *
 * The constants live here, next to their board's memmap, so a re-import of
 * third_party/tinyusb cannot silently bring the wrong chip's value back: the
 * shim's _Static_assert (tusb_shim.c) pins this against RESETS_USBCTRL.
 */
#ifndef TOS_USB_RESETS_H
#define TOS_USB_RESETS_H

#define TOS_RESETS_USBCTRL 0x01000000u   /* RP2040: bit 24 */

#endif
