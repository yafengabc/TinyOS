/* pico-sdk compatibility layer for the vendored TinyUSB RP2040 port.
 *
 * TinyUSB's dcd_rp2040.c / rp2040_usb.c are taken verbatim from upstream, so
 * they expect the pico-sdk environment: hardware/structs/usb.h, irq_*(),
 * reset_block(), time_us_32(), panic().  This file implements the handful of
 * those that are functions; the rest are macros in port_rp2/include.
 *
 * The RISC-V port does not take the USBCPU interrupt: Hazard3 routes
 * external lines through its own custom CSRs, and the poll path in
 * usb_cdc.c drains the controller directly.  irq_set_enabled() is
 * therefore a no-op and irq_add_shared_handler() only remembers the
 * handler.  reset_block()/unreset_block_wait() are real: dcd_init() resets
 * the USB controller before configuring it. */

#include "platform.h"
#include "board.h"
#include "rp2350rv_memmap.h"
#include "console.h"
#include "usb_cdc.h"

#include "hardware/irq.h"
#include "hardware/resets.h"
#include "pico/platform.h"

/* ---- interrupt plumbing -------------------------------------------------- */

static void (*s_usb_isr)(void);

void irq_add_shared_handler(uint num, void (*handler)(void), uint8_t order_priority)
{
    (void)order_priority;
    if (num != USBCTRL_IRQ)
        return;
    s_usb_isr = handler;
}

void irq_remove_handler(uint num, void (*handler)(void))
{
    if (num == USBCTRL_IRQ && s_usb_isr == handler)
        s_usb_isr = 0;
}

void irq_set_enabled(uint num, bool enabled)
{
    (void)num;
    (void)enabled;
    /* USB runs in IRQ mode since TOS_RV_USB_IRQ=1: the trap handler's
     * plat_irq_foreign() hands line 14 to usb_cdc_irq_task() (see
     * board/rp2350rv/platform.c).  The controller's INTE register is still
     * programmed by dcd_init(); usb_hw->ints is the polled fallback's
     * liveness probe. */
}

/* ---- peripheral reset ---------------------------------------------------- */

void reset_block(uint32_t bits)
{
    mmio_write32(RESETS_BASE + 0x2000u + RESETS_RESET_OFFSET, bits);
}

void unreset_block(uint32_t bits)
{
    mmio_write32(RESETS_BASE + 0x3000u + RESETS_RESET_OFFSET, bits);
}

void unreset_block_wait(uint32_t bits)
{
    unreset_block(bits);
    /* Bounded like every other wait in this port: a block whose clock is not
     * running never reports RESET_DONE, and an unbounded spin there turns a
     * USB problem into an unbootable board. */
    for (uint32_t spins = 0; spins < 1000000u; spins++) {
        if ((mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & bits) == bits)
            return;
    }
}

/* ---- misc ---------------------------------------------------------------- */

/* See pico/platform.h: reporting 1 selects the port's conservative EP_ABORT
 * path.  The >= 2 branch busy-waits for ABORT_DONE, which is an RP2040 B2
 * errata workaround and not something this RP2350 port has verified. */
uint8_t rp2040_chip_version(void)
{
    return 1;
}

/* TinyUSB calls panic() on states it considers unreachable.  The kernel
 * already provides a formatting panic() in kernel/console.c, which is what
 * pico/platform.h declares here - no shim implementation on purpose, since a
 * second one would be a duplicate symbol and would print unformatted text. */

/* <stdio.h> is included unconditionally by tusb_common.h, and its debug path
 * formats a lookup key with snprintf.  The kernel's formatter is kvsnprintf,
 * so this is the thin adapter rather than a second implementation. */
int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list ap;
    int n;

    __builtin_va_start(ap, fmt);
    n = kvsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

