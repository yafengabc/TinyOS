#include "platform.h"

/* Board: QEMU virt, rv32.
 *
 * RAM starts at 0x80000000.  The firmware (or our own entry point) loads
 * the kernel there, so APP_BASE sits above the kernel image + heap. */

const uint32_t PLAT_KERNEL_BASE = 0x80000000;
const uint32_t PLAT_KERNEL_MAX  = 0x80300000;   /* kernel may use 3 MiB */

const uint32_t PLAT_HEAP_BASE   = 0x80300000;
const uint32_t PLAT_HEAP_END    = 0x88000000;   /* rest of the 128 MiB */

const uint32_t PLAT_APP_BASE       = 0x80400000;
const uint32_t PLAT_APP_MAX        = 0x87F00000;
const uint32_t PLAT_APP_STACK_TOP  = 0x88000000;
const uint32_t PLAT_APP_HEAP_BASE  = 0x84000000;
