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
 * PLAT_APP_GRAIN is the step it is rounded to: 32 KiB leaves 16 KiB for the
 * image and the heap once the 8 KiB stack and the 4 KiB guard are accounted
 * for, and the 136 KiB of app area holds eight of those.  (It used to say
 * 16 KiB guard and a 32 KiB stack here, which cannot fit at all - every exec
 * was refused, unnoticed because no Pico had a console yet.) */

const uint32_t PLAT_KERNEL_BASE = 0x20000000u;   /* SRAM reserved for the
                                                   kernel image + .bss      */

const uint32_t PLAT_APP_MAX    = 0x20042000u;   /* 264 KiB SRAM ends here   */
const uint32_t PLAT_RAM_TOP    = 0x20042000u;   /* same end: all SRAM is ours */
const uint32_t PLAT_RAM_BASE   = 0x20000000u;   /* .data/.bss start of SRAM  */
const uint32_t PLAT_APP_GRAIN  = 0x00004000u;   /* 16 KiB per step: 8 fit    */
/* Stack at the top of every task window, and the hole left under it.  See
 * kernel/platform.h for the four conditions this pair has to satisfy.
 * 8 KiB by default; a program that needs more asks for it in its image
 * (TOS_STACK()), up to the 16 KiB grain. */
const uint32_t PLAT_STACK_BYTES = 8u * 1024u;
const uint32_t PLAT_HEAP_GUARD  = 4u * 1024u;

/* littlefs starts at flash 0x80000 (512 KiB), the same offset as the
 * ESP32-C3 and RP2350 (rpLfsOffset in internal/build/uf2.go).  A Pico has
 * 2 MiB of QSPI flash, so the partition covers the remaining 1.5 MiB;
 * the shipped image is still only 300 KiB and ramdisk_init() grows the
 * mounted filesystem over the rest. */
const uint32_t PLAT_LFS_LBA = 0x80000u / 512u;   /* 1024 */
const uint32_t PLAT_LFS_BLOCKS = 384;            /* 1.5 MiB */

/* One CPU: these boards have a single core.  The SMP build sets 2 and
 * start.S brings the second hart up (see kernel/cpu.c). */
const uint32_t PLAT_MAX_CPUS = 1;
