#include "platform.h"

/* Board: generic PC / QEMU i386 with 128 MiB of RAM.
 *
 * The memory map is documented in docs/PLATFORM-DESIGN.md.  Constants are
 * plain globals rather than #defines so that a board can be selected at
 * link time, and so other translation units can reference them. */

const uint32_t PLAT_KERNEL_BASE = 0x00010000;   /* loaded by the boot sector   */
const uint32_t PLAT_KERNEL_MAX  = 0x00400000;   /* must not reach the heap     */

const uint32_t PLAT_HEAP_BASE   = 0x00400000;   /* first managed frame         */
const uint32_t PLAT_HEAP_END    = 0x04000000;   /* 60 MiB managed              */

const uint32_t PLAT_APP_BASE       = 0x08048000;
const uint32_t PLAT_APP_MAX        = 0x30000000;
const uint32_t PLAT_APP_STACK_TOP  = 0x10000000;
const uint32_t PLAT_APP_HEAP_BASE  = 0x20000000;
