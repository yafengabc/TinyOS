#ifndef _HARDWARE_IRQ_H
#define _HARDWARE_IRQ_H

/* TinyOS shim: pico-sdk's IRQ plumbing reduced to what TinyUSB's RP2040 port
 * calls.  The vector table is shared by every ARM board in this tree, so the
 * actual dispatch goes through g_usb_irq_handler (arch/arm/platform.c) rather
 * than naming a symbol from the TinyUSB side. */

#include "pico/types.h"

#define USBCTRL_IRQ 14

/* Only USBCTRL has a wrapped stub in the shared vector table; other numbers
 * are accepted so the shim links, but enabling them does nothing. */
void irq_add_shared_handler(uint num, void (*handler)(void), uint8_t order_priority);
void irq_remove_handler(uint num, void (*handler)(void));
void irq_set_enabled(uint num, bool enabled);

#define PICO_SHARED_IRQ_HANDLER_LOWEST_ORDER_PRIORITY  0x00
#define PICO_SHARED_IRQ_HANDLER_DEFAULT_ORDER_PRIORITY 0x80
#define PICO_SHARED_IRQ_HANDLER_HIGHEST_ORDER_PRIORITY 0xff

#endif
