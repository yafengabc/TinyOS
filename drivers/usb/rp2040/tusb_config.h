#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

/* TinyUSB build configuration for the TinyOS RP2040 port.
 *
 * Only the device stack and the CDC-ACM class are compiled (the vendored
 * tree in third_party/tinyusb contains nothing else), so every other class
 * switch stays off and contributes no code.
 *
 * CFG_TUSB_MCU is OPT_MCU_RP2040, which is the native value here (the port
 * was originally written for the RP2350, which had to borrow it because the
 * vendored TinyUSB revision predates an RP2350 MCU id).  Chip-specific
 * differences live in drivers/usb/rp2040/usb_cdc.c and the shim layer, not
 * in the MCU id. */

#define CFG_TUSB_MCU                OPT_MCU_RP2040
#define CFG_TUSB_OS                 OPT_OS_NONE
#define CFG_TUSB_RHPORT0_MODE       (OPT_MODE_DEVICE | OPT_MODE_FULL_SPEED)

/* Level 2 = the usbd state machine's own log (SETUP / SET_CONFIGURATION /
 * enumeration failures).  Off by default; turn to 1 for a quieter run, and
 * remember every message costs UART time inside the USB interrupt. */
#define CFG_TUSB_DEBUG              2
#define CFG_TUSB_DEBUG_PRINTF       usb_dbg_printf

/* 64-byte control endpoint: full speed allows 8/16/32/64, and 64 is what
 * every other descriptor in this port assumes. */
#define CFG_TUD_ENDPOINT0_SIZE      64

/* ---- CDC-ACM ------------------------------------------------------------ */

#define CFG_TUD_CDC                 1

/* One bulk IN + one bulk OUT, plus one interrupt IN for the notification
 * endpoint.  That is 3 of the controller's 16 endpoints. */
#define CFG_TUD_CDC_EP_BUFSIZE      64

/* The FIFOs live in .bss and are pure overhead for a console: 128 bytes is
 * two full-speed packets, enough to cover one tick of output without
 * stalling the writer, and 256 bytes cheaper than the 512-byte default. */
#define CFG_TUD_CDC_RX_BUFSIZE      128
#define CFG_TUD_CDC_TX_BUFSIZE      128

/* No audio/HID/MSC/MIDI/vendor class is compiled in. */
#define CFG_TUD_DFU                 0
#define CFG_TUD_HID                 0
#define CFG_TUD_MSC                 0
#define CFG_TUD_MIDI                0
#define CFG_TUD_VENDOR              0
#define CFG_TUD_AUDIO               0
#define CFG_TUD_VIDEO               0

#endif
