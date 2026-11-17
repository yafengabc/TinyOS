#include "platform.h"
#include "board_config.h"

/* Board: Raspberry Pi RP2350 (Cortex-M33, ARMv8-M Secure).
 *
 * Code runs in place from XIP flash at 0x10000000.  Unlike RP2040 there is
 * no stage-2: the boot ROM reads the vector table from flash offset 0 and
 * finds our PICOBIN IMAGE_DEF in the first 4 KiB (board/rp2350-generic/image_def.S).
 *
 * The chip has 520 KiB of SRAM at 0x20000000.  The first 128 KiB holds the
 * kernel's .data/.bss/stack, and the striped banks up to 0x20080000 (512 KiB)
 * are task windows: 384 KiB at a 16 KiB grain is 24 windows, three times the
 * RP2040's eight.  The two 4 KiB scratch banks at 0x20080000 are left alone.
 *
 * The M33 has ARMv8-M's PMSAv8 MPU, and the port uses it (TOS_ARM_MPU_V8):
 * arch/arm/platform.c programs PRBAR/PRLAR instead of RBAR/RASR, so a task
 * sees only its own window - read-only code, writable data/heap/stack, and a
 * privileged-only guard page under the stack.  The MPU is switched on by the
 * first task switch, not during boot. */

const uint32_t PLAT_KERNEL_BASE = 0x20000000u;

const uint32_t PLAT_APP_MAX    = 0x20080000u;   /* 512 KiB striped SRAM   */
const uint32_t PLAT_RAM_TOP    = 0x20080000u;
const uint32_t PLAT_RAM_BASE   = 0x20000000u;
const uint32_t PLAT_APP_GRAIN  = 0x00001000u;   /* 4 KiB, 96 windows       */

const uint32_t PLAT_STACK_BYTES = 4u * 1024u;
const uint32_t PLAT_HEAP_GUARD  = 4u * 1024u;

/* littlefs starts 512 KiB into the flash (see rpLfsOffset in
 * internal/build/uf2.go) and runs to the end of the chip.  The ATK board's
 * W25Q32 is 4 MiB, measured by XIP address wrap-around (+2 MiB reads 0xFF,
 * +4 MiB reads the vector table again), which gives 896 blocks.  That is a
 * board fact now, in board/rp2350-generic/board.conf: a Pico 2 - or any board with a
 * different flash part - changes two numbers instead of editing this file.
 * The flashed image stays 300 KiB (75 blocks); ramdisk_init() grows the
 * mounted filesystem to this count on first boot.
 *
 * TOS_LFS_OFFSET/TOS_LFS_BLOCKS come from the generated board_config.h and
 * board_config_check() refuses to boot if they disagree with these two. */
#if TOS_HAS_FLASH
const uint32_t PLAT_LFS_LBA = TOS_LFS_OFFSET / 512u;
const uint32_t PLAT_LFS_BLOCKS = TOS_LFS_BLOCKS;
#else
/* A board with no flash claim (board/rp2350-generic) mounts a RAM disk that
 * is linked into the kernel: there is no flash LBA, and the block count is
 * the image's own 300 KiB.  The numbers mirror board/qemu-virt, which is the
 * other embedded-filesystem board in the tree. */
const uint32_t PLAT_LFS_LBA = 0;
const uint32_t PLAT_LFS_BLOCKS = 75;
#endif

/* One core unless the build asks for two.  The `atk-dnr2350am-smp` target defines
 * TOS_MAX_CPUS=2, which also doubles the kernel stack pool in
 * arch/arm/start.S and makes cpu_init() launch core 1 through the SIO FIFO.
 * PLAT_MAX_CPUS and the build define have to agree: cpu_init() panics if the
 * pool size is not exactly KSTACK_BYTES * PLAT_MAX_CPUS. */
#ifndef TOS_MAX_CPUS
#define TOS_MAX_CPUS 1
#endif
const uint32_t PLAT_MAX_CPUS = TOS_MAX_CPUS;
