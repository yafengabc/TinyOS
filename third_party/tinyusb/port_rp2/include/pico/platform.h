#ifndef _PICO_PLATFORM_H
#define _PICO_PLATFORM_H

/* TinyOS shim: pico-sdk's platform.h surface that TinyUSB relies on.
 *
 *   panic()/hard_assert()  - library-level fatal errors
 *   rp2040_chip_version()  - the port gates the EP_ABORT errata workaround
 *   __not_in_flash_func()  - placement attributes, no-ops here because the
 *                            kernel is not yet splitting RAM/ROM text
 *
 * assert() is deliberately a no-op: the port calls it on invariants that are
 * already checked by TU_ASSERT in the common layer, and a real assert would
 * pull in a panic path from the USB interrupt handler. */

#include "types.h"
#include <assert.h>

#ifndef __unused
#define __unused __attribute__((unused))
#endif

/* pico-sdk spells these __not_in_flash_func(name) / __time_critical_func(name)
 * without a trailing semicolon; the definition has to expand to the function
 * name so the declaration that follows still parses. */
#define __not_in_flash_func(func_name)          func_name
#define __time_critical_func(func_name)         func_name
#define __no_inline_not_in_flash_func(func_name) func_name

#define __breakpoint() __asm__ volatile("bkpt #0")

/* pico-sdk's escape hatch for handing a volatile peripheral pointer to code
 * that wants a plain one (the port needs it for the SETUP packet in DPRAM).
 * Going through uintptr_t drops the qualifier without a cast warning. */
#define remove_volatile_cast(__type, __x) ((__type)(uintptr_t)(__x))

/* pico-sdk calibrates this against the system clock; the port only uses it to
 * space out pull-up toggling, so a conservative lower bound is enough: the
 * loop below runs three cycles per iteration on Cortex-M (subs, nop, bne).
 * It lives here rather than in hardware/sync.h because rp2040_usb.c calls it
 * without including that header - pico.h is what brought it in upstream. */
static inline void busy_wait_at_least_cycles(uint32_t minimum_cycles)
{
    uint32_t n = minimum_cycles / 3u + 1u;
    __asm__ volatile("1: subs %0, #1\n\tbne 1b" : "+r"(n) :: "memory");
}

void panic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

#define hard_assert(x)                                                     \
    do {                                                                   \
        if (!(x))                                                          \
            panic("hard_assert failed: %s:%d", __FILE__, __LINE__);        \
    } while (0)

/* The EP_ABORT path is gated on "chip version >= 2" and busy-waits for
 * ABORT_DONE.  That errata fix targets RP2040 B2 silicon; this port runs on
 * an RP2350, where the documented workaround is different and a never-coming
 * ABORT_DONE would hang the interrupt handler.  Reporting version 1 selects
 * the conservative path that only resets the buffer control words. */
uint8_t rp2040_chip_version(void);

#endif
