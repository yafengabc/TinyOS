#include "platform.h"
#include "board_config.h"

/* Board: Raspberry Pi RP2350 (Hazard3 RISC-V, PMP).
 *
 * Same SRAM map as the ARM half (board/rp2350-generic/layout.c), which this
 * file shadows so the RISC-V port can differ where its protection unit does.
 *
 * The Hazard3 uses PMP, not an MPU: the stack is its own NAPOT region whose
 * base must be aligned to its size, and kernel stack_request_ok() therefore
 * requires a stack hint that divides the window grain.  The on-board tools
 * (csc, rcc) ask for 16 KiB, so the grain here is 16 KiB - 384 KiB of task
 * windows is 24 of them, the number the ARM layout's comment already assumes.
 * board_check.c compares these three numbers with the generated
 * board_config.h and refuses to boot on a mismatch, which is why the
 * RP2350-RV32 board configs carry the same grain and stack_align = 1. */

const uint32_t PLAT_KERNEL_BASE = 0x20000000u;

const uint32_t PLAT_APP_MAX    = 0x20080000u;   /* 512 KiB striped SRAM   */
const uint32_t PLAT_RAM_TOP    = 0x20080000u;
const uint32_t PLAT_RAM_BASE   = 0x20000000u;
const uint32_t PLAT_APP_GRAIN  = 0x00004000u;   /* 16 KiB, 24 windows     */

const uint32_t PLAT_STACK_BYTES = 4u * 1024u;
const uint32_t PLAT_HEAP_GUARD  = 4u * 1024u;

/* littlefs geometry, same contract as the ARM layout: the numbers come from
 * board.conf through board_config.h and board_config_check() compares them. */
#if TOS_HAS_FLASH
const uint32_t PLAT_LFS_LBA = TOS_LFS_OFFSET / 512u;
const uint32_t PLAT_LFS_BLOCKS = TOS_LFS_BLOCKS;
#else
const uint32_t PLAT_LFS_LBA = 0;
const uint32_t PLAT_LFS_BLOCKS = 75;
#endif

/* One core unless the build asks for two (rpi-pico2-rv32-smp).  PLAT_MAX_CPUS
 * and TOS_MAX_CPUS have to agree: cpu_init() panics otherwise. */
#ifndef TOS_MAX_CPUS
#define TOS_MAX_CPUS 1
#endif
const uint32_t PLAT_MAX_CPUS = TOS_MAX_CPUS;
