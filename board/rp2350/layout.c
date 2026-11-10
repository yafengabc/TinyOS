#include "platform.h"

/* Board: Raspberry Pi RP2350 (Cortex-M33, ARMv8-M Secure).
 *
 * Code runs in place from XIP flash at 0x10000000.  Unlike RP2040 there is
 * no stage-2: the boot ROM reads the vector table from flash offset 0 and
 * finds our PICOBIN IMAGE_DEF in the first 4 KiB (board/rp2350/image_def.S).
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
const uint32_t PLAT_APP_GRAIN  = 0x00004000u;   /* 16 KiB, 24 windows      */

const uint32_t PLAT_STACK_BYTES = 8u * 1024u;
const uint32_t PLAT_HEAP_GUARD  = 4u * 1024u;

/* littlefs starts at flash 0x80000 (512 KiB), same offset as the ESP32-C3
 * and the RP2040 - see rpLfsOffset in internal/build/uf2.go.  The board's
 * W25Q32 is 4 MiB (measured by XIP address wrap-around: +2 MiB reads 0xFF,
 * +4 MiB reads the vector table again), so the partition runs from there
 * to the end of the chip: (0x400000 - 0x80000) / 4096 = 896 blocks =
 * 3.5 MiB.  The flashed image itself stays 300 KiB (75 blocks);
 * ramdisk_init() grows the mounted filesystem to this count on first boot. */
const uint32_t PLAT_LFS_LBA = 0x80000u / 512u;   /* 1024 */
const uint32_t PLAT_LFS_BLOCKS = 896;            /* 3.5 MiB */

/* One core unless the build asks for two.  The `rp2350smp` target defines
 * TOS_MAX_CPUS=2, which also doubles the kernel stack pool in
 * arch/arm/start.S and makes cpu_init() launch core 1 through the SIO FIFO.
 * PLAT_MAX_CPUS and the build define have to agree: cpu_init() panics if the
 * pool size is not exactly KSTACK_BYTES * PLAT_MAX_CPUS. */
#ifndef TOS_MAX_CPUS
#define TOS_MAX_CPUS 1
#endif
const uint32_t PLAT_MAX_CPUS = TOS_MAX_CPUS;
