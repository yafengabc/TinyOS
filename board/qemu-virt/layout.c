#include "platform.h"

/* Board: QEMU virt, rv32.
 *
 * RAM starts at 0x80000000.  QEMU's -kernel loads the ELF at its link
 * address, so the kernel image sits at the bottom of RAM, exactly like it
 * does on the PC target. */

const uint32_t PLAT_KERNEL_BASE = 0x80000000;
const uint32_t PLAT_KERNEL_MAX  = 0x80400000;   /* image + .bss stay in 4 MiB */
const uint32_t PLAT_RAM_TOP     = 0x88000000;   /* the 128 MiB QEMU is given */
const uint32_t PLAT_RAM_BASE    = 0x80000000;   /* RAM starts at the kernel */

/* Each task gets a 1 MiB window: 128 MiB of RAM / 8 = 16 MiB of app space
 * is far more than the handful of programs in the filesystem need, and the
 * bump allocator in process.c never runs out. */
const uint32_t PLAT_APP_BASE   = 0x80400000;
const uint32_t PLAT_APP_MAX    = 0x81400000;
const uint32_t PLAT_APP_WINDOW = 0x00100000;   /* 1 MiB per task */
/* Stack at the top of every task window, and the hole left under it.  See
 * kernel/platform.h for the four conditions this pair has to satisfy. */
const uint32_t PLAT_STACK_BYTES = 32u * 1024u;
const uint32_t PLAT_HEAP_GUARD  = 16u * 1024u;


/* The filesystem image is linked into the kernel (.data.ramdisk), so the
 * partition starts at block 0 of the device. */
const uint32_t PLAT_LFS_LBA = 0;
const uint32_t PLAT_LFS_BLOCKS = 75;   /* 300 KiB, unchanged */

/* How many harts this kernel was built for.  QEMU virt is the one board
 * that can be *given* more than one (-smp N), so its answer comes from the
 * build: the rv32smp target passes -DTOS_MAX_CPUS=2 and the kernel links a
 * second trap vector and a second kernel-stack block for it.  Every other
 * board in the tree is a real single-core part and hardcodes 1. */
const uint32_t PLAT_MAX_CPUS = TOS_MAX_CPUS;
