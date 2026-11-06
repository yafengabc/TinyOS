#ifndef TINYOS_STD_ASSERT_SHIM_H
#define TINYOS_STD_ASSERT_SHIM_H

/* Minimal freestanding assert.h.
 *
 * assert() is a no-op on purpose.  The vendored TinyUSB controller driver
 * asserts on invariants it has already validated through TU_ASSERT in the
 * common layer, and a real assert would pull a panic path into the USB
 * interrupt handler - the worst possible place to discover a message-printing
 * bug.  Anything that genuinely must be fatal goes through hard_assert()
 * (pico/platform.h) or TU_ASSERT, both of which reach panic(). */

#include "types.h"

#undef assert
#define assert(cond) ((void)0)

#endif
