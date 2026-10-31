#include "platform.h"

/* Board: QEMU virt, rv32.
 *
 * RAM starts at 0x80000000.  The firmware (or our own entry point) loads
 * the kernel there, so APP_BASE sits above the kernel image. */

const uint32_t PLAT_KERNEL_BASE = 0x80000000;
const uint32_t PLAT_KERNEL_MAX  = 0x80300000;   /* image + .bss stay in 3 MiB */

const uint32_t PLAT_APP_BASE   = 0x80400000;
const uint32_t PLAT_APP_MAX    = 0x87F00000;
const uint32_t PLAT_APP_WINDOW = 0x00200000;   /* 2 MiB per task */
