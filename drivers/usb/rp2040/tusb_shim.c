/* pico-sdk compatibility layer for the vendored TinyUSB RP2040 port.
 *
 * TinyUSB's dcd_rp2040.c / rp2040_usb.c are taken verbatim from upstream, so
 * they expect the pico-sdk environment: hardware/structs/usb.h, irq_*(),
 * reset_block(), time_us_32(), panic().  This file implements the handful of
 * those that are functions; the rest are macros in port_rp2/include.
 *
 * The pieces that are NOT stubs and matter for correctness:
 *
 *   irq_add_shared_handler() - the shared ARM vector table dispatches through
 *       g_usb_irq_handler (arch/arm/platform.c), because arch/arm/start.S is
 *       compiled for every ARM board and must not name a USB symbol.
 *
 *   irq_set_enabled() - opens the NVIC gate for USBCTRL_IRQ using the same byte
 *       write to IPR that the previous driver needed: the priority register
 *       for USBCTRL_IRQ is not 4-byte aligned, and a 32-bit access there faults.
 *
 *   reset_block()/unreset_block_wait() - dcd_init() resets the USB controller
 *       before configuring it.  This has to be a real reset: a warm reboot
 *       after a crash otherwise inherits armed endpoints from the previous
 *       run and the host sees a device that never answers. */

#include "platform.h"
#include "board.h"
#include "scs.h"
#include "console.h"
#include "usb_cdc.h"

#include "hardware/irq.h"
#include "hardware/resets.h"
#include "pico/platform.h"

/* ---- interrupt plumbing -------------------------------------------------- */

/* dcd_init() hands us TinyUSB's controller handler through this call; the
 * shared vector table only knows one function pointer, so the pointer it sees
 * is a trampoline that runs the controller handler and then TinyUSB's task.
 * pico-sdk does exactly the same thing in stdio_usb: the task has to run
 * inside the interrupt, because a control-transfer stage answered one tick
 * later is already too late for the host. */
static void (*s_usb_isr)(void);

/* Counted so the bring-up trace can tell a healthy interrupt rate (a few per
 * millisecond during enumeration, then tens per second) from a storm that
 * starves the main loop. */
volatile uint32_t g_usb_irq_count;

static void usb_irq_trampoline(void)
{
    uint32_t ints = mmio_read32(0x50110098u);   /* USBCTRL INTS, pre-handler */

    /* reset_usb_boot() is spinning in wfi after writing scratch[4..7] and
     * enabling the watchdog timer.  The tick never decrements that counter
     * on this port, so use a live USB interrupt (SOF keeps arriving) to
     * force WATCHDOG_CTRL.TRIGGER.  The bootrom then sees scratch[4] and
     * jumps to its _usb_boot_reboot_wrapper instead of waiting forever. */
    extern int plat_bootsel_force_check(void);
    if (plat_bootsel_force_check()) {
        mmio_write32(0x40058000u, 0x80000000u);  /* WATCHDOG_CTRL.TRIGGER */
        for (;;)
            ;
    }

    g_usb_irq_count++;
    usb_cdc_trace_snap();       /* raw registers, before anything clears them */
    /* The controller handler races usb_cdc_putc() on the other core - run
     * it inside the cross-core USB critical section.  The section masks
     * PRIMASK first, so this ISR can never spin on a lock held by the
     * thread it just pre-empted: a lock holder is always interrupt-masked. */
    {
        uint32_t lk = usb_cdc_lock();
        if (s_usb_isr)
            s_usb_isr();        /* dcd_int_handler(): latch SIE events */
        usb_cdc_unlock(lk);
    }
    /* Erratum RP2040-E15 (B0/B1): the SIE needs 800 us of LS_J after a bus
     * reset before it will accept packets.  The SDK fix is compiled out of
     * the vendored TinyUSB port; request our equivalent here, but let the
     * poll path run it: waiting out the reset's 10-15 ms of SE0 inside this
     * interrupt blocked the very SETUP packet the host sends next. */
    if (ints & 0x00001000u)     /* USB_INTS_BUS_RESET_BITS */
        usb_cdc_e15_request();
    /* While E15 is pending or holding J, the task must not consume the
     * queued BUS_RESET event yet - pico-sdk runs the fix after
     * dcd_event_bus_reset() is queued and before tud_task(), and so does
     * usb_cdc_poll().  It is called from the tick (or from the tight early
     * enumeration loop), so the delay is one millisecond at most. */
    if (usb_cdc_e15_pending())
        return;
    /* The task has to run in this same interrupt.  usb_cdc.c owns the
     * re-entrancy guard (g_in_task + a PRIMASK window), so this is safe even
     * when the IRQ lands inside the tick's own tud_task() call.
     *
     * Without this call the RP2040 never enumerates: the only remaining
     * tud_task() was the 1 ms tick, and every control-transfer stage was
     * answered a full millisecond late, so the host gave up after the first
     * descriptor and reset the port in a loop.  The RP2350 port gets away
     * with the deferred task at 150 MHz; this one cannot. */
    usb_cdc_irq_task();
}

void irq_add_shared_handler(uint num, void (*handler)(void), uint8_t order_priority)
{
    (void)order_priority;
    if (num != USBCTRL_IRQ)
        return;
    s_usb_isr = handler;
    g_usb_irq_handler = usb_irq_trampoline;
}

void irq_remove_handler(uint num, void (*handler)(void))
{
    if (num == USBCTRL_IRQ && s_usb_isr == handler) {
        s_usb_isr = 0;
        g_usb_irq_handler = 0;
    }
}

void irq_set_enabled(uint num, bool enabled)
{
    if (num != USBCTRL_IRQ)
        return;

    if (enabled) {
        /* IPR holds one priority byte per IRQ, so USBCTRL_IRQ lands on a byte
         * address that is not 4-byte aligned; a 32-bit store there takes a
         * HardFault on Cortex-M.  Priority 0 keeps USB above SysTick (0x20),
         * because a SET_ADDRESS status stage has microseconds of budget. */
        *(volatile uint8_t *)ARM_NVIC_IPR(num) = 0x00u;
        mmio_write32(ARM_NVIC_ISER(0),
                     mmio_read32(ARM_NVIC_ISER(0)) | (1u << num));
    } else {
        mmio_write32(ARM_NVIC_ICER(0),
                     mmio_read32(ARM_NVIC_ICER(0)) | (1u << num));
    }
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

