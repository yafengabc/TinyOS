#include "platform.h"

/* Board: ESP32-C3 (any 400 KiB SRAM / 4 MiB flash module).
 *
 * Addresses use the instruction-bus window throughout: the chip aliases one
 * physical SRAM at 0x3FC80000 (data) and 0x40380000 (instruction), and the
 * real chip refuses to fetch from the 0x3FC8 window (verified, see
 * docs/PLATFORM-DESIGN.md ?8.1).  Everything - kernel, windows, PMP - lives
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

/* Image + .bss: the kernel owns everything from PLAT_KERNEL_BASE up to the
 * link-time __kernel_end, and the task area starts at the first 16 KiB
 * boundary after it.  The kernel code itself is XIP in flash; this region is
 * only .data/.bss/heap.
 *
 * The split is derived at link time (__app_base in kernel-xip.ld), so every
 * 16 KiB the heap or kernel stack does not use is one more window: a window
 * is a whole program.  The link only fails if the kernel leaves no room for
 * a single window. */

/* Task windows are carved out of the rest of SRAM above __app_base, in
 * 4 KiB steps: a 1 KiB tool takes 16 KiB, a 10 KiB program 24 KiB, the
 * editor 32, and the rest of the area stays usable by whatever comes next.
 * The step can be smaller than the 8 KiB stack because this PMP has TOR
 * matching: the stack is its own TOR region, so nothing here needs the
 * window to be a multiple of the stack size (see plat_task_protect and
 * plat_window_needs_stack_alignment).  The Python interpreter's 128 KiB window plus the shell's
 * 32 KiB still fit.  Stack at the top of every window, image below it,
 * guard hole between image and stack; see kernel/platform.h for the
 * constraints this grain has to satisfy. */
const uint32_t PLAT_APP_MAX    = 0x403DC000;
const uint32_t PLAT_APP_GRAIN  = 0x00001000;   /* 4 KiB per step  */
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
 * docs/PLATFORM-DESIGN.md ?8.7).  PLAT_LFS_BLOCKS sizes it to the rest of
 * the 4 MiB flash: (0x400000 - 0x80000) / 4096 = 896 blocks = 3.5 MiB. */
const uint32_t PLAT_LFS_LBA = 0x80000 / 512;   /* 1024 */
const uint32_t PLAT_LFS_BLOCKS = (0x400000u - 0x80000u) / 4096u;

/* One CPU: these boards have a single core.  The SMP build sets 2 and
 * start.S brings the second hart up (see kernel/cpu.c). */
const uint32_t PLAT_MAX_CPUS = 1;
