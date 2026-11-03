#include "platform.h"

/* Board: Raspberry Pi RP2040 (Cortex-M0+).
 *
 * Code runs in place from XIP flash at 0x10000100 (the address the on-board
 * stage-2 jumps to, just past the 256-byte boot stage-2 at 0x10000000).  The
 * 264 KiB of on-chip SRAM at 0x20000000 holds .data/.bss and the kernel
 * stack; user programs are relocated into SRAM above them.
 *
 * The RP2040 has no MMU and no MPU: a user task runs unprivileged (so a fault
 * cannot corrupt the kernel) but with the whole flat address space visible,
 * exactly like the x86 port.  A window is therefore just an allocation, and
 * PLAT_APP_GRAIN is the step it is rounded to: 32 KiB leaves 12 KiB for the
 * image and the heap once the 16 KiB stack and the 4 KiB guard are
 * accounted for, and the 136 KiB of app area holds eight of those.  (It used
 * to say 16 KiB guard and a 32 KiB stack here, which cannot fit at all -
 * every exec was refused, unnoticed because no Pico had a console yet.) */

const uint32_t PLAT_KERNEL_BASE = 0x20000000u;   /* SRAM reserved for the
                                                   kernel image + .bss      */
const uint32_t PLAT_KERNEL_MAX  = 0x20020000u;   /* 128 KiB: kernel .data/
                                                   .bss/stack must end below */

const uint32_t PLAT_APP_BASE   = 0x20020000u;
const uint32_t PLAT_APP_MAX    = 0x20042000u;   /* 264 KiB SRAM ends here   */
const uint32_t PLAT_RAM_TOP    = 0x20042000u;   /* same end: all SRAM is ours */
const uint32_t PLAT_RAM_BASE   = 0x20000000u;   /* .data/.bss start of SRAM  */
const uint32_t PLAT_APP_GRAIN  = 0x00004000u;   /* 16 KiB per step: 8 fit    */
/* Stack at the top of every task window, and the hole left under it.  See
 * kernel/platform.h for the four conditions this pair has to satisfy. */
const uint32_t PLAT_STACK_BYTES = 16u * 1024u;
const uint32_t PLAT_HEAP_GUARD  = 4u * 1024u;

/* The littlefs image is flashed 1 MiB in (see emitRP2040UF2 in build.go). */
const uint32_t PLAT_LFS_LBA = 0x100000u / 512u;
const uint32_t PLAT_LFS_BLOCKS = 75;   /* 300 KiB, unchanged */

/* One CPU: these boards have a single core.  The SMP build sets 2 and
 * start.S brings the second hart up (see kernel/cpu.c). */
const uint32_t PLAT_MAX_CPUS = 1;
