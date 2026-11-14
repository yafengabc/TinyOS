#include "platform.h"

/* Board: Raspberry Pi RP2350, Hazard3 RISC-V cores.
 *
 * Code runs in place from the flash XIP window at 0x10000000.  The boot ROM
 * finds board/rp2350rv/image_def.S in the first 4 KiB and jumps to _start,
 * so the kernel's RAM footprint is only .data + .bss - the 300 KiB littlefs
 * image stays in flash at 0x80000 and is read through XIP.
 *
 * SRAM is 520 KiB at 0x20000000.  The first 128 KiB hold the kernel, and
 * the striped banks up to 0x20080000 (512 KiB) are task windows, the same
 * split the ARM port uses; the two 4 KiB scratch banks at the top are left
 * alone.
 *
 * There is no MMU.  RV32 uses PMP (arch/rv32/platform.c) to give a task its
 * code, data/heap and stack and nothing else. */

const uint32_t PLAT_KERNEL_BASE = 0x20000000u;

const uint32_t PLAT_APP_MAX    = 0x20080000u;   /* 512 KiB striped SRAM   */
const uint32_t PLAT_RAM_TOP    = 0x20080000u;
const uint32_t PLAT_RAM_BASE   = 0x20000000u;
const uint32_t PLAT_APP_GRAIN  = 0x00001000u;   /* 4 KiB, 96 windows       */

const uint32_t PLAT_STACK_BYTES = 4u * 1024u;
const uint32_t PLAT_HEAP_GUARD  = 4u * 1024u;

/* littlefs starts at flash 0x80000 (512 KiB), the same offset the ARM image
 * and the RP2040 use - see rpLfsOffset in internal/build/uf2.go.  The
 * flashed image is 300 KiB (600 sectors = 75 littlefs blocks); keeping
 * PLAT_LFS_BLOCKS at 75 means lfs_fs_grow() is a no-op on first mount, so
 * the first RISC-V boot does not need a flash erase.  Growing the partition
 * over the rest of the chip is a later change. */
const uint32_t PLAT_LFS_LBA = 0x80000u / 512u;   /* 1024 */
const uint32_t PLAT_LFS_BLOCKS = 75;             /* 300 KiB */

/* The first milestone is single-hart.  Hazard3 can do SMP, but that needs
 * the second core's launch protocol and a per-core mtimecmp, so it is a
 * separate target when it exists - exactly like rp2350 vs rp2350smp. */
#ifndef TOS_MAX_CPUS
#define TOS_MAX_CPUS 1
#endif
const uint32_t PLAT_MAX_CPUS = TOS_MAX_CPUS;
