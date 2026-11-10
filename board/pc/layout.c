#include "platform.h"

/* Board: generic PC / QEMU i386 with 128 MiB of RAM.
 *
 * The memory map is documented in docs/PLATFORM-DESIGN.md.  Constants are
 * plain globals rather than #defines so that a board can be selected at
 * link time, and so other translation units can reference them. */

const uint32_t PLAT_KERNEL_BASE = 0x00010000;   /* loaded by the boot sector   */
const uint32_t PLAT_RAM_TOP     = 0x08000000;   /* the 128 MiB QEMU is given */
const uint32_t PLAT_RAM_BASE    = 0x00010000;   /* RAM starts under the kernel */

/* Flat model (no paging, CR0.PG = 0): user windows must live inside the
 * 128 MiB of RAM that QEMU is started with, and every task gets its own
 * window, sized to its image in 64 KiB steps, so a synchronous exec never
 * overwrites its parent. */
const uint32_t PLAT_APP_MAX    = 0x08000000;
const uint32_t PLAT_APP_GRAIN  = 0x00010000;   /* 64 KiB per step */
/* Stack at the top of every task window, and the hole left under it.  See
 * kernel/platform.h for the four conditions this pair has to satisfy. */
const uint32_t PLAT_STACK_BYTES = 32u * 1024u;
const uint32_t PLAT_HEAP_GUARD  = 16u * 1024u;


/* The boot sector loads the kernel from LBA 1 and the filesystem follows
 * it (BootSectors + KernelSectors = 181, see internal/build). */
const uint32_t PLAT_LFS_LBA = 181;
const uint32_t PLAT_LFS_BLOCKS = 75;   /* 300 KiB, unchanged */

/* One CPU: these boards have a single core.  The SMP build sets 2 and
 * start.S brings the second hart up (see kernel/cpu.c). */
const uint32_t PLAT_MAX_CPUS = 1;
