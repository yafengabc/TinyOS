#ifndef TINYOS_STD_STDIO_SHIM_H
#define TINYOS_STD_STDIO_SHIM_H

/* Minimal freestanding stdio.h.
 *
 * Nothing in the kernel uses stdio: console output goes through kprintf()
 * (kernel/console.h) and the platform's plat_console_* hooks.  The header
 * exists because common/tusb_common.h includes it unconditionally, and it
 * declares nothing on purpose - a vendored library that actually calls
 * printf() should fail at link time instead of silently pulling in a second
 * formatting implementation. */

#include "types.h"

/* The one formatting entry point vendored code may legitimately need.  It is
 * implemented in board/rp2350/tusb_shim.c on top of kvsnprintf(); declaring
 * it here keeps <stdio.h> honest without dragging in a second formatter. */
int snprintf(char *buf, size_t size, const char *fmt, ...);

#endif
