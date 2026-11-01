#include "platform.h"

/* Board: ESP32-C3 (any 400 KiB SRAM / 4 MiB flash module).
 *
 * Addresses use the instruction-bus window throughout: the chip aliases one
 * physical SRAM at 0x3FC80000 (data) and 0x40380000 (instruction), and the
 * real chip refuses to fetch from the 0x3FC8 window (verified, see
 * docs/PLATFORM-DESIGN.md §8.1).  Everything - kernel, windows, PMP - lives
 * in 0x4038 space; data accesses work on both aliases, so picking the
 * instruction one costs nothing.
 *
 * Usable SRAM ends at 0x403DC710: above that sit the ROM's shared download
 * buffers (0x3FCCAE00..0x3FCD C710 DRAM-side) and its stack.  The boundary
 * values are from Espressif's own linker scripts (soc/esp32c3/memory.h in
 * Zephyr/esp-idf), cross-checked in docs/ZEPHYR-REFERENCE.md. */

const uint32_t PLAT_KERNEL_BASE = 0x40380000;

/* Image + .bss must stay below this.  176 KiB is generous for the ~110 KiB
 * the rv32 kernel currently occupies and keeps the window area page tidy. */
const uint32_t PLAT_KERNEL_MAX  = 0x403AC000;

/* Three 64 KiB windows: 32 KiB stack at the top, a guard hole, then image
 * and sbrk heap below.  One window is the shell, one for a foreground
 * program, one spare for exec chains. */
const uint32_t PLAT_APP_BASE   = 0x403AC000;
const uint32_t PLAT_APP_MAX    = 0x403DC000;
const uint32_t PLAT_APP_WINDOW = 0x00010000;   /* 64 KiB per task */
/* Stack at the top of every task window, and the hole left under it.  See
 * kernel/platform.h for the four conditions this pair has to satisfy. */
const uint32_t PLAT_STACK_BYTES = 16u * 1024u;
const uint32_t PLAT_HEAP_GUARD  = 16u * 1024u;


/* The littlefs partition lives on SPI flash at offset 0x100000, served by
 * plat_block_read/write/erase in platform.c (ROM reader + writer, SPI1
 * hardware erase; see docs/PLATFORM-DESIGN.md §8.7). */
const uint32_t PLAT_LFS_LBA = 0x100000 / 512;
