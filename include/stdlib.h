#ifndef TINYOS_STD_STDLIB_H
#define TINYOS_STD_STDLIB_H

/* Minimal freestanding stdlib.h.
 *
 * The kernel's allocator lives behind kmalloc()/kfree(); this header exists
 * only so vendored sources that include <stdlib.h> for size_t/NULL compile.
 * No allocator is declared, so nothing here can accidentally bypass the
 * kernel heap accounting. */

#include "types.h"

#endif
