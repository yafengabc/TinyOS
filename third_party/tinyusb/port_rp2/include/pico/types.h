#ifndef _PICO_TYPES_H
#define _PICO_TYPES_H

/* TinyOS shim: pico-sdk's pico/types.h exists mostly to define `uint`, which
 * the SDK uses everywhere as a shortcut for unsigned int.  The TinyOS type
 * set (kernel/types.h) is deliberately stdint-flavoured and has no such
 * alias, so it is added here rather than polluting the kernel namespace. */

#include "types.h"
#include <stdbool.h>

typedef unsigned int uint;

#endif
