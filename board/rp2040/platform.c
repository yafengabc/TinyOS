#include "platform.h"
#include "board.h"
#include "arm_memmap.h"
#include "usb_cdc.h"
#include "console.h"
#include "string.h"

/* Board: Raspberry Pi RP2040 (dual Cortex-M0+, ARMv6-M).
 *
 * The RP2040 has no MMU and no MPU, so the flat "no memory protection" model
 * from the x86 port applies unchanged (see plat_task_protect in
 * arch/arm/platform.c): a task runs unprivileged but sees the whole address
 * space.  There is also no BASEPRI, no ldrex/strex, and only a simplified
 * HardFault (no CFSR/MMFAR/BFAR), so fault reporting just prints the PC.
 *
 * Boot flow: the on-chip boot ROM loads the 256-byte stage-2
 * (board/rp2040/boot2.bin) from flash offset 0, verifies a CRC-32/MPEG-2 over
 * its first 252 bytes and jumps to 0x10000100 (the reset vector in our vector
 * table).  The stage-2 leaves clk_sys and clk_peri running from the 12 MHz
 * crystal (XOSC), which is the clock source this board assumes everywhere
 * (UART baud and SysTick rate).  Exception vectors are NOT repointed by the
 * stage-2, so we set VTOR ourselves in plat_board_early.
 *
 * Flash: it runs in place from XIP at 0x10000000, so reads go straight through
 * the XIP mapping.  Writes and erases go through the boot ROM's flash API
 * (resolved from the boot ROM function table), which drives the QSPI serial
 * command state and must be followed by rom_flash_flush_cache() so XIP sees the
 * new data.  The littlefs image is concatenated at flash offset 0x80000 by the
 * UF2 packer, which matches PLAT_LFS_LBA * 512 == 0x80000. */

/* ---- boot ROM flash API ----------------------------------------------- */

/* The function signatures are from the RP2040 boot ROM header
 * (bootrom.h, BSD-3-Clause, Raspberry Pi Ltd).  The boot ROM stores a 16-bit
 * function-table pointer at BOOTROM_FUNC_TABLE_OFFSET (0x14) and a pointer to
 * its own lookup routine at BOOTROM_TABLE_LOOKUP_OFFSET (0x18); both are
 * 16-bit addresses into the (<=64 KiB) boot ROM. */
typedef void (*rp_rom_erase_t)(uint32_t addr, size_t count,
                               uint32_t block_size, uint8_t block_cmd);
typedef void (*rp_rom_program_t)(uint32_t addr, const uint8_t *data, size_t count);
typedef void (*rp_rom_flush_t)(void);
typedef void *(*rp_rom_lookup_t)(uint16_t *table, uint32_t code);

static rp_rom_erase_t   g_rom_erase;
static rp_rom_program_t g_rom_program;
static rp_rom_flush_t   g_rom_flush;
static int              g_rom_ready;   /* 0 until all three are resolved */

/* Read a 16-bit value at `addr` and treat it as a pointer.  The boot ROM is
 * small enough that every pointer it stores is 16 bits. */
static void *rp_rom_hword(uint32_t addr)
{
    return (void *)(uintptr_t)(*(volatile uint16_t *)addr);
}

static void *rp_rom_func(uint32_t code)
{
    rp_rom_lookup_t lookup = (rp_rom_lookup_t)rp_rom_hword(BOOTROM_TABLE_LOOKUP_OFFSET);
    uint16_t *functab = (uint16_t *)rp_rom_hword(BOOTROM_FUNC_TABLE_OFFSET);
    if (!lookup || !functab)
        return NULL;
    /* The table returns a Thumb-callable address; OR in the Thumb bit so the
     * value is safe to bx regardless of how the ROM stored it. */
    return (void *)((uintptr_t)lookup(functab, code) | 1u);
}

/* ---- console: USB CDC-ACM ------------------------------------------------ */

/* The Pico board has no soldered UART pins in the target setup and the only
 * connector is USB, so the console lives on the hand-rolled CDC stack in
 * usb_cdc.c: plugging the board into a PC enumerates a COM port with no
 * extra hardware.  The polled EP0/bulk model matches the kernel's tick-driven
 * console exactly (no USB interrupt is registered). */

/* Defined in the boot section below; the console turns it off once the
 * host has enumerated. */
static void led_set(int on);

void plat_console_init(void)
{
    usb_cdc_console_init();
    /* Enumerated: the LED has done its job (see led_init). */
    if (usb_cdc_configured())
        led_set(0);
}

void plat_console_putc(char c)
{
    usb_cdc_putc(c);
}

void plat_console_flush(void)
{
    usb_cdc_flush();
}

int plat_console_tx_room(void)
{
    return usb_cdc_tx_room();
}

int plat_console_getchar(void)
{
    return usb_cdc_getchar();
}

/* ---- SysTick ----------------------------------------------------------- */

/* The stage-2 leaves clk_sys (and therefore the SysTick input) at 12 MHz, so
 * a reload of CLK/hz gives `hz` ticks per second.  The counter is started
 * here with the interrupt masked; plat_irq_enable() later unmasks it (same
 * ordering the rv32 port gets from mstatus.MIE). */

static volatile uint64_t g_ticks;
static uint32_t g_reload;

void plat_timer_init(uint32_t hz)
{
    g_reload = ARM_SYSTICK_CLK / hz;
    g_ticks = 0;

    mmio_write32(ARM_SYSTICK_LOAD, g_reload - 1u);
    mmio_write32(ARM_SYSTICK_VAL, 0);
    /* ENABLE | CLKSOURCE (processor clock).  TICKINT (interrupt) stays clear. */
    mmio_write32(ARM_SYSTICK_CTRL, 5u);
}

uint64_t plat_ticks(void)
{
    return g_ticks;
}

uint32_t plat_tick_hz(void)
{
    return ARM_SYSTICK_CLK / g_reload;
}

uint32_t plat_uptime_ms(void)
{
    return (uint32_t)((g_ticks * 1000u) / (uint64_t)plat_tick_hz());
}

/* Clock: the tick gives whole periods, SysTick's live down-counter gives
 * the position inside the current one, so resolution is 1/12 MHz (~83 ns)
 * rather than 10 ms.  Clamped against the wrap that happens between the
 * reload and the tick interrupt. */
uint64_t plat_now_ns(void)
{
    static uint64_t last_ns;
    uint32_t load = g_reload;
    uint32_t val  = mmio_read32(ARM_SYSTICK_VAL) & 0xFFFFFFu;
    uint32_t into = (load > val) ? (load - val) : 0u;
    uint64_t cycles = g_ticks * (uint64_t)load + into;
    /* 12 MHz does not divide into whole cycles per microsecond, so work in
     * thousandths: ns = cycles * 1000000 / CLK, split to avoid overflow. */
    uint64_t per_ms = ARM_SYSTICK_CLK / 1000u;
    uint64_t ns = (cycles / per_ms) * 1000000ull
                + ((cycles % per_ms) * 1000000ull) / per_ms;

    if (ns < last_ns)
        ns = last_ns;
    last_ns = ns;
    return ns;
}

void plat_timer_tick(void)
{
    g_ticks++;
    /* SysTick reloads itself; nothing to re-arm. */
}

/* ---- block device: NOR flash via boot ROM ------------------------------ */

/* The build tool concatenates the littlefs image into the flash at offset
 * 0x80000 (see emitRP2040UF2 in internal/build/uf2.go), which is exactly
 * PLAT_LFS_LBA * 512.  lba passed by ramdisk.c is already global
 * (PLAT_LFS_LBA + sector), so the flash byte address is simply lba * 512.
 * Erase operates on 4 KiB sectors (8 littlefs sectors); the ROM function
 * requires the address aligned to 4 KiB and the count a multiple of 4 KiB. */

#define RP_FLASH_SECTOR   4096u
#define RP_SECTOR_ERASE_CMD 0x20u          /* W25Qxx 4 KiB sector erase   */

int plat_block_read(uint32_t lba, uint32_t count, void *buf)
{
    /* Reads go straight through the XIP mapping; the cache serves them once
     * flushed after the most recent write. */
    const void *src = (const void *)(XIP_BASE + lba * 512u);
    memcpy(buf, src, count * 512u);
    return 0;
}

int plat_block_erase(uint32_t lba, uint32_t count)
{
    uint32_t addr, end, was_enabled;

    if (!g_rom_ready)
        return -1;

    /* Round the range out to 4 KiB sector boundaries for the ROM call. */
    addr = (lba * 512u) & ~(RP_FLASH_SECTOR - 1u);
    end  = ((lba + count) * 512u + RP_FLASH_SECTOR - 1u) & ~(RP_FLASH_SECTOR - 1u);

    /* The ROM functions drive the QSPI in serial command mode and busy-wait
     * while the flash is in use; an interrupt whose handler lives in XIP
     * (e.g. SysTick) must not fire mid-operation, so take the lock and
     * restore the prior interrupt state afterwards. */
    __asm__ volatile("mrs %0, primask" : "=r"(was_enabled));
    __asm__ volatile("cpsid i" ::: "memory");
    g_rom_erase(addr, end - addr, RP_FLASH_SECTOR, RP_SECTOR_ERASE_CMD);
    g_rom_flush();
    if (!(was_enabled & 1u))
        __asm__ volatile("cpsie i" ::: "memory");
    return 0;
}

int plat_block_write(uint32_t lba, uint32_t count, const void *buf)
{
    uint32_t addr, was_enabled;

    if (!g_rom_ready)
        return -1;

    /* lba is always 512-aligned and count*512 is a multiple of 256, satisfying
     * the ROM program alignment rules.  The bounce buffer in ramdisk.c is in
     * SRAM, so the ROM reads its source from RAM (never from XIP). */
    addr = lba * 512u;

    __asm__ volatile("mrs %0, primask" : "=r"(was_enabled));
    __asm__ volatile("cpsid i" ::: "memory");
    g_rom_program(addr, (const uint8_t *)buf, count * 512u);
    g_rom_flush();
    if (!(was_enabled & 1u))
        __asm__ volatile("cpsie i" ::: "memory");
    return 0;
}

uint32_t plat_block_size(void)
{
    return 512;
}

uint32_t plat_block_count(void)
{
    /* The whole 1.5 MiB littlefs partition is addressable, even though the
     * flashed image only occupies its first 300 KiB; ramdisk_init() grows
     * the filesystem over the rest (see PLAT_LFS_BLOCKS in layout.c). */
    return PLAT_LFS_BLOCKS * 8u;   /* 3072 sectors = 1.5 MiB */
}

/* ---- boot -------------------------------------------------------------- */

/* The on-board LED (GPIO25 through SIO on a plain Pico) is the only
 * diagnostic a board with no console can offer, so it is used as a boot
 * flag: it comes on the moment our image starts executing and goes off once
 * the host has enumerated the CDC device.  That splits "nothing flashed or
 * the CRC failed" (LED dark) from "kernel runs but USB is silent" (LED
 * stays on) without a debugger - the console cannot report its own
 * absence. */
#define RP_LED_GPIO 25u

static void led_init(void)
{
    uint32_t pad = mmio_read32(PADS_BANK0_BASE + 4u * RP_LED_GPIO);

    /* Pad: input enable on (bit 6), output disable off (bit 7).  The bank
     * itself is already out of reset - the boot ROM's stage-2 uses QSPI
     * pins from the same bank. */
    mmio_write32(PADS_BANK0_BASE + 4u * RP_LED_GPIO, (pad | (1u << 6)) & ~(1u << 7));
    /* IO_BANK0 GPIO25_CTRL: status at +0x00, control at +0x04.  FUNCSEL 5
     * is SIO on every RP2040 pin. */
    mmio_write32(IO_BANK0_BASE + 0x04u + 8u * RP_LED_GPIO, 5u);

    mmio_write32(SIO_BASE + 0x010u, 0);              /* GPIO_OUT: off     */
    mmio_write32(SIO_BASE + 0x024u, 1u << RP_LED_GPIO); /* GPIO_OE_SET     */
}

static void led_set(int on)
{
    mmio_write32(SIO_BASE + (on ? 0x014u : 0x018u), 1u << RP_LED_GPIO);
}

void plat_board_early(void)
{
    led_init();
    led_set(1);

    /* Repoint exceptions at our vector table (the stage-2 leaves VTOR at the
     * boot ROM).  Must happen before any interrupt/fault can be taken. */
    mmio_write32(ARM_VTOR, XIP_BASE + 0x100u);

    /* Defensive core-1 park.  The boot ROM normally holds core 1 in its own
     * wait-for-FIFO loop, so it never executes our image; if it ever were
     * started (e.g. a future multicore launch) it spins here instead of
     * corrupting the single kernel image in SRAM. */
    if (mmio_read32(SIO_CPUID) != 0) {
        for (;;)
            __asm__ volatile("wfe");
    }

    plat_console_init();
}

void plat_board_init(void)
{
    /* Resolve the boot ROM flash API.  If any entry is missing (for example a
     * simulator without a boot ROM) the block device degrades to read-only and
     * the littlefs write self-test reports FAILED instead of crashing. */
    g_rom_erase   = (rp_rom_erase_t)rp_rom_func(ROM_FUNC_FLASH_RANGE_ERASE);
    g_rom_program = (rp_rom_program_t)rp_rom_func(ROM_FUNC_FLASH_RANGE_PROGRAM);
    g_rom_flush   = (rp_rom_flush_t)rp_rom_func(ROM_FUNC_FLASH_FLUSH_CACHE);
    g_rom_ready   = (g_rom_erase && g_rom_program && g_rom_flush) ? 1 : 0;
}

/* Called from plat_irq_enable() the first time interrupts come on.  TICKINT
 * stays clear until then so a tick cannot arrive before the scheduler exists. */
void plat_timer_irq_unmask(void)
{
    uint32_t ctrl = mmio_read32(ARM_SYSTICK_CTRL);
    if (!(ctrl & 2u))
        mmio_write32(ARM_SYSTICK_CTRL, ctrl | 2u);
}

/* ---- power -------------------------------------------------------------- */

void plat_shutdown(void)
{
    console_shutdown();
    plat_panic_halt();
}

void plat_reboot(void)
{
    console_shutdown();

    /* SYSRESETREQ resets everything but the debug domain, and the boot ROM
     * then loads stage-2 from flash again - the same cold path as power-on.
     * Not exercised yet: the Pico has no console wired up (see the notes on
     * the missing second USB CDC port), so this is textbook ARMv6-M, not
     * hardware-verified. */
    mmio_write32(ARM_AIRCR, ARM_AIRCR_VECTKEY | ARM_AIRCR_SYSRESETREQ);

    plat_panic_halt();
}

/* SIO_CPUID reads 0 on core 0 and 1 on core 1 - the only thing that tells
 * the two cores apart before the scheduler exists. */
uint32_t plat_board_cpu_id(void)
{
    return mmio_read32(SIO_CPUID);
}
