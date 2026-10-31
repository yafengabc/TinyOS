#include "platform.h"

/* Board: generic PC / QEMU i386 with 128 MiB of RAM.
 *
 * The memory map is documented in docs/PLATFORM-DESIGN.md.  Constants are
 * plain globals rather than #defines so that a board can be selected at
 * link time, and so other translation units can reference them. */

const uint32_t PLAT_KERNEL_BASE = 0x00010000;   /* loaded by the boot sector   */
const uint32_t PLAT_KERNEL_MAX  = 0x00400000;   /* image + .bss must stay below */

/* Flat model (no paging, CR0.PG = 0): user windows must live inside the
 * 128 MiB of RAM that QEMU is started with, and every task gets its own
 * window so a synchronous exec never overwrites its parent. */
const uint32_t PLAT_APP_BASE   = 0x01000000;
const uint32_t PLAT_APP_MAX    = 0x08000000;
const uint32_t PLAT_APP_WINDOW = 0x00400000;   /* 4 MiB per task */
