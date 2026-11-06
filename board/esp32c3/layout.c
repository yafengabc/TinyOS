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

/* One past the last byte the kernel may use.  Usable SRAM ends at
 * 0x403DC710; above that sit the ROM's shared download buffers and its
 * stack (see docs/ZEPHYR-REFERENCE.md).  From the kernel base that is
 * 378,640 bytes, i.e. the 384 KiB SRAM block minus what the ROM keeps. */
const uint32_t PLAT_RAM_BASE   = 0x40380000;   /* SRAM, instruction alias */
const uint32_t PLAT_RAM_TOP    = 0x403DC710;

/* Image + .bss must stay below this: 64 KiB.  It was 176 KiB while the code
 * half of the kernel also lived in SRAM, then 128 KiB once XIP moved .text
 * and .rodata to flash - and 128 KiB turned out to be an arbitrary number
 * that left 50.8 KiB of dead space between __kernel_end and the first task
 * window, which neither the kernel nor a window could ever use.
 *
 * 64 KiB is 4 grains, i.e. a boundary the task area can start on.  The
 * kernel's .bss is 48.6 KiB after streaming exec (16 KiB kernel stack,
 * 8 KiB arena, 8 process slots), which leaves ~15.4 KiB of headroom.  The
 * build asserts the fit in kernel-xip.ld, so growth past it is a link error
 * rather than pid 1's image being overwritten.
 *
 * Every number here is a lever on the task area: each 16 KiB the kernel
 * gives back is one more window, and a window is a whole program. */
const uint32_t PLAT_KERNEL_MAX  = 0x40390000;

/* Task windows are carved out of the remaining 304 KiB (19 x 16 KiB) and
 * sized to their image, in 16 KiB steps: a shell takes 32 KiB, the editor
 * 32, a 1 KiB tool 16, and the rest of the area stays usable by whatever
 * comes next.  Stack at the top of every window, image below it, guard hole
 * between image and stack; see kernel/platform.h for the constraints this
 * grain has to satisfy. */
const uint32_t PLAT_APP_BASE   = 0x40390000;
const uint32_t PLAT_APP_MAX    = 0x403DC000;
const uint32_t PLAT_APP_GRAIN  = 0x00004000;   /* 16 KiB per step */
/* Stack at the top of every task window, and the hole left under it.  See
 * kernel/platform.h for the four conditions this pair has to satisfy.
 *
 * 8 KiB is the default, not a ceiling: a program that needs more says so in
 * its image (TOS_STACK(), up to the grain), and `ps`'s stk column is what
 * says whether the default was right.  Every real program measured under
 * 1 KiB, and the default is the floor of every window, so halving it is
 * what lets a 4 KiB image run in 16 KiB instead of 32.
 *
 * The guard is one page, not the 16 KiB it used to be: it is the floor of
 * every window, and with variable windows it decides whether a 32 KiB
 * window has 12 KiB of image space or 0.  One page still catches a stack
 * that runs off its bottom - the hole is simply not covered by any PMP
 * entry, so any store into it faults, and the write that overflows lands
 * there first unless a single frame is larger than the hole. */
const uint32_t PLAT_STACK_BYTES = 8u * 1024u;
const uint32_t PLAT_HEAP_GUARD  = 4u * 1024u;


/* The littlefs partition lives on SPI flash at offset 0x80000 (the
 * kernel keeps the first 512 KiB), served by plat_block_read/write/erase
 * in platform.c (ROM reader + writer, SPI1 hardware erase; see
 * docs/PLATFORM-DESIGN.md §8.7).  PLAT_LFS_BLOCKS sizes it to the rest of
 * the 4 MiB flash: (0x400000 - 0x80000) / 4096 = 896 blocks = 3.5 MiB. */
const uint32_t PLAT_LFS_LBA = 0x80000 / 512;   /* 1024 */
const uint32_t PLAT_LFS_BLOCKS = (0x400000u - 0x80000u) / 4096u;

/* One CPU: these boards have a single core.  The SMP build sets 2 and
 * start.S brings the second hart up (see kernel/cpu.c). */
const uint32_t PLAT_MAX_CPUS = 1;
