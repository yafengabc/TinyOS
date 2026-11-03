#include "platform.h"

/* Board: QEMU mps2-an385 (Cortex-M3).
 *
 * Code lives at 0 (the loader places the image there and the core reads the
 * vector table from it); SRAM starts at 0x20000000.  User programs are
 * relocated into SRAM above the kernel, exactly like the other ports.
 *
 * mps2-an385 has 4 MiB of SRAM and no MMU: the split below is what the bump
 * allocator in kernel/process.c carves the task windows out of.  The kernel
 * image (.data holds the littlefs ramdisk) plus .bss and the 32 KiB kernel
 * stack currently end around 0x2005e298, so 512 KiB is a comfortable
 * reserve; kernel.ld asserts the image really stays under it.
 */

const uint32_t PLAT_KERNEL_BASE = 0x00000000;
const uint32_t PLAT_KERNEL_MAX  = 0x20080000;   /* 512 KiB of SRAM for image + .bss */
const uint32_t PLAT_RAM_TOP     = 0x20400000;   /* end of the 4 MiB of SRAM */
/* Code is linked at 0 and copied to SRAM, so the kernel's RAM does not
 * start at PLAT_KERNEL_BASE here. */
const uint32_t PLAT_RAM_BASE    = 0x20000000;

const uint32_t PLAT_APP_BASE   = 0x20080000;
const uint32_t PLAT_APP_MAX    = 0x20400000;   /* end of the 4 MiB of SRAM */
const uint32_t PLAT_APP_WINDOW = 0x00040000;   /* 256 KiB per task: 14 windows */
/* Stack at the top of every task window, and the hole left under it.  See
 * kernel/platform.h for the four conditions this pair has to satisfy. */
const uint32_t PLAT_STACK_BYTES = 32u * 1024u;
const uint32_t PLAT_HEAP_GUARD  = 16u * 1024u;


/* The filesystem image is linked into the kernel (.data.ramdisk). */
const uint32_t PLAT_LFS_LBA = 0;
const uint32_t PLAT_LFS_BLOCKS = 75;   /* 300 KiB, unchanged */

/* One CPU: these boards have a single core.  The SMP build sets 2 and
 * start.S brings the second hart up (see kernel/cpu.c). */
const uint32_t PLAT_MAX_CPUS = 1;
