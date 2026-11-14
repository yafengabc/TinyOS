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
 * PLAT_APP_GRAIN is the step it is rounded to.
 *
 * The task area is not a fixed reservation: kernel.ld puts __app_base at the
 * first 16 KiB boundary after the real __kernel_end, so every byte the kernel
 * does not use is user memory.  With the current kernel (about 46 KiB: 8 KiB
 * heap, 8 KiB kernel stack, 8 process slots) that leaves 0x2000c000..
 * 0x20042000 = 216 KiB, i.e. 54 windows of 4 KiB - plenty for a board whose
 * whole SRAM is 264 KiB.  Growing the kernel by a 16 KiB boundary costs one
 * window's worth, which is the intended trade. */

const uint32_t PLAT_KERNEL_BASE = 0x20000000u;   /* SRAM reserved for the
                                                   kernel image + .bss      */

const uint32_t PLAT_APP_MAX    = 0x20042000u;   /* 264 KiB SRAM ends here   */
const uint32_t PLAT_RAM_TOP    = 0x20042000u;   /* same end: all SRAM is ours */
const uint32_t PLAT_RAM_BASE   = 0x20000000u;   /* .data/.bss start of SRAM  */
const uint32_t PLAT_APP_GRAIN  = 0x00001000u;   /* 4 KiB per step          */
/* Stack at the top of every task window, and the hole left under it.  See
 * kernel/platform.h for the four conditions this pair has to satisfy.
 * 8 KiB by default; a program that needs more asks for it in its image
 * (TOS_STACK()), up to the 16 KiB grain. */
const uint32_t PLAT_STACK_BYTES = 4u * 1024u;
const uint32_t PLAT_HEAP_GUARD  = 4u * 1024u;

/* littlefs starts at flash 0x80000 (512 KiB), the same offset as the
 * ESP32-C3 and RP2350 (rpLfsOffset in internal/build/uf2.go).  A Pico has
 * 2 MiB of QSPI flash, so the partition covers the remaining 1.5 MiB;
 * the shipped image is still only 300 KiB and ramdisk_init() grows the
 * mounted filesystem over the rest. */
const uint32_t PLAT_LFS_LBA = 0x80000u / 512u;   /* 1024 */
const uint32_t PLAT_LFS_BLOCKS = 384;            /* 1.5 MiB */

/* One CPU by default; the rp2040smp target builds with TOS_MAX_CPUS=2 and
 * start.S brings the second core up from the ROM's FIFO wait loop (the
 * board's plat_start_cpu). */
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
const uint32_t PLAT_MAX_CPUS = TOS_MAX_CPUS;
#else
const uint32_t PLAT_MAX_CPUS = 1;
#endif
