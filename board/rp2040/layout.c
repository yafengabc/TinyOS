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
 * exactly like the x86 port.  PLAT_APP_WINDOW must therefore be large enough
 * to hold the image, the heap, the 16 KiB stack guard (PLAT_HEAP_GUARD) and
 * the stack. */

const uint32_t PLAT_KERNEL_BASE = 0x20000000u;   /* SRAM reserved for the
                                                   kernel image + .bss      */
const uint32_t PLAT_KERNEL_MAX  = 0x20020000u;   /* 128 KiB: kernel .data/
                                                   .bss/stack must end below */

const uint32_t PLAT_APP_BASE   = 0x20020000u;
const uint32_t PLAT_APP_MAX    = 0x20042000u;   /* 264 KiB SRAM ends here   */
const uint32_t PLAT_APP_WINDOW = 0x00008000u;   /* 32 KiB per task          */

/* The littlefs image is flashed 1 MiB in (see emitRP2040UF2 in build.go). */
const uint32_t PLAT_LFS_LBA = 0x100000u / 512u;
