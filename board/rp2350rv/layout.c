#include "platform.h"
#include "board_config.h"

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
 * and the RP2040 use - see rpLfsOffset in internal/build/uf2.go.
 *
 * 896 blocks = 3.5 MiB, the same partition the ARM port uses, so both images
 * report the same `df` and the RISC-V build gets the whole flash instead of
 * 300 KiB.  The packed image is smaller (704 sectors), so the first mount
 * after a flash calls lfs_fs_grow(): it commits a new superblock and lets
 * blocks be erased lazily on first use, exactly as the ARM port has done
 * since it shipped.  That is a boot-time flash write, which is why this was
 * deferred until the write path had the 200-round dd run behind it.
 *
 * Before this, the packer wrote a 352 KiB image while the partition claimed
 * 300 KiB, and `df` reported 105% used: the superblock's block count and
 * PLAT_LFS_BLOCKS disagreed. */
const uint32_t PLAT_LFS_LBA = TOS_LFS_OFFSET / 512u;
const uint32_t PLAT_LFS_BLOCKS = TOS_LFS_BLOCKS;

/* The first milestone is single-hart.  Hazard3 can do SMP, but that needs
 * the second core's launch protocol and a per-core mtimecmp, so it is a
 * separate target when it exists - exactly like rp2350 vs rp2350smp. */
#ifndef TOS_MAX_CPUS
#define TOS_MAX_CPUS 1
#endif
const uint32_t PLAT_MAX_CPUS = TOS_MAX_CPUS;
