#include "platform.h"
#include "board.h"
#include "console.h"

/* Board: ESP32-C3.
 *
 * Espressif UART0 (not 16550), TIMG0 for the tick, INTMTX to route it to
 * the CPU, and the TG0 watchdog that the ROM leaves running and that we
 * switch off before it resets us one second in (verified on the real
 * chip: a probe that just spins gets TG0WDT_SYS_RST every ~1 s).
 *
 * Register addresses and bit positions are from the esp32c3 register
 * headers vendored in third_party/esp32c3-soc/ (Apache-2.0, Espressif);
 * the UART bits were additionally exercised on the real chip by the
 * probe under scripts/esp-c3/. */

#define UART0_BASE   0x60000000u
#define TIMG0_BASE   0x6001F000u
#define INTMTX_BASE  0x600C2000u

/* ---- UART0 ----------------------------------------------------------- */

#define UART_FIFO_REG   0x00
#define UART_STATUS_REG 0x1C
#define UART_TXFIFO_CNT_V 0x3FF
#define UART_TXFIFO_CNT_S 16
#define UART_RXFIFO_CNT_V 0x3FF
#define UART_RXFIFO_CNT_S 0

/* The ROM configures UART0 for the console at 115200 8N1 before it jumps
 * to the image, so there is nothing to programme - just drain the FIFO
 * and go. */

static int uart_tx_count(void)
{
    return (mmio_read32(UART0_BASE + UART_STATUS_REG) >> UART_TXFIFO_CNT_S)
           & UART_TXFIFO_CNT_V;
}

static int uart_rx_count(void)
{
    return (mmio_read32(UART0_BASE + UART_STATUS_REG) >> UART_RXFIFO_CNT_S)
           & UART_RXFIFO_CNT_V;
}

/* Dual console: UART0 (CH343 on GPIO20/21) and the hardware USB-Serial/JTAG
 * CDC (native USB connector).  Output goes to both, input is accepted from
 * whichever has data, so the board stays reachable over the wired line while
 * the USB port is being exercised.  The USJ path drops bytes when no host is
 * reading; the UART keeps everything. */

void usj_init(void);
void usj_putc(char c);
int  usj_getchar(void);
void usj_irq_init(void);
void usj_irq_service(void);
void usj_flush(void);

void plat_console_init(void)
{
    /* The ROM already left UART0 up; just bring up the USB CDC pads. */
    usj_init();
}

void plat_console_putc(char c)
{
    /* The FIFO is 128 deep; 100 leaves headroom for our own CRLF pairs. */
    while (uart_tx_count() >= 100)
        ;
    mmio_write32(UART0_BASE + UART_FIFO_REG, (uint8_t)c);
    usj_putc(c);
}

void plat_console_flush(void)
{
    while (uart_tx_count() > 0)
        ;
    usj_flush();
}

/* Echo paces itself against this instead of blocking in putc(): the drain
 * loop above would otherwise stop us from reading the 128-byte receive FIFO
 * for as long as the echo takes, which is longer than a tick for any burst
 * over ~100 characters. */
int plat_console_tx_room(void)
{
    int used = uart_tx_count();

    return used >= 100 ? 0 : 100 - used;
}

int plat_console_getchar(void)
{
    if (uart_rx_count() > 0)
        return mmio_read32(UART0_BASE + UART_FIFO_REG) & 0xFF;
    return usj_getchar();
}


/* ---- TIMG0 tick + INTMTX --------------------------------------------- */

/* 80 MHz APB clock, divided down to 1 MHz; 10000 counts per tick = 100 Hz. */
#define TIMG_CLK_HZ    80000000u
#define TIMG_DIVIDER   80u
#define TICK_HZ        100u

#define T0CONFIG_REG   (TIMG0_BASE + 0x00)
#define T0LO_REG       (TIMG0_BASE + 0x04)
#define T0HI_REG       (TIMG0_BASE + 0x08)
#define T0UPDATE_REG   (TIMG0_BASE + 0x0C)
#define T0ALARMLO_REG  (TIMG0_BASE + 0x10)
#define T0ALARMHI_REG  (TIMG0_BASE + 0x14)
#define T0LOADLO_REG   (TIMG0_BASE + 0x18)
#define T0LOADHI_REG   (TIMG0_BASE + 0x1C)
#define T0LOAD_REG     (TIMG0_BASE + 0x20)
#define WDTCONFIG0_REG (TIMG0_BASE + 0x48)
#define WDTFEED_REG    (TIMG0_BASE + 0x60)
#define WDTWPROTECT_REG (TIMG0_BASE + 0x64)
#define INT_ENA_REG    (TIMG0_BASE + 0x70)
#define INT_CLR_REG    (TIMG0_BASE + 0x7C)
#define REGCLK_REG     (TIMG0_BASE + 0xFC)

#define REGCLK_CLK_EN       (1u << 31)  /* 0 = registers R/W disabled */
#define REGCLK_TIMER_ACTIVE (1u << 30)  /* default 1 */
#define REGCLK_WDT_ACTIVE   (1u << 29)  /* default 1 */

#define T0_EN          (1u << 31)
#define T0_INCREASE    (1u << 30)
#define T0_AUTORELOAD  (1u << 29)
#define T0_ALARM_EN    (1u << 10)   /* self-clearing on alarm */
#define T0_DIVIDER_S   13
#define T0_DIVIDER_M   0xFFFFu

#define WDT_WKEY       0x50D83AA1u
#define WDT_EN         (1u << 31)

/* RTC_CNTL watchdog (the "super watchdog" esptool leaves armed). */
#define RTC_WDT_BASE            0x60008000u
#define RTC_WDT_CONFIG0_REG     0x90u
#define RTC_WDT_WPROTECT_REG    0xA8u

/* The SUPER watchdog on top of those: it fires ~3 s after boot
 * (rst SUPER_WDT_RST) even with the two watchdogs above off. */
#define RTC_SWD_CONF_REG        0xACu
#define RTC_SWD_WPROTECT_REG    0xB0u
#define RTC_SWD_DISABLE         (1u << 30)
#define RTC_SWD_WKEY            0x8F1D312Au

/* Software system reset.  OPTIONS0 bit 31 is write-only: setting it resets
 * the digital domain (the RTC domain survives) and the CPU restarts at the
 * ROM, which boots whatever sits at flash offset 0 - the same path esptool
 * takes after flashing.  Nothing to unlock, nothing to acknowledge: the
 * write either resets the chip or, if the bit were wrong, does nothing at
 * all and plat_panic_halt() keeps the machine stopped. */
#define RTC_CNTL_OPTIONS0_REG   0x000u
#define RTC_CNTL_SW_SYS_RST     (1u << 31)

/* INTMTX (interrupt matrix).  Source 32 is TG0 timer 0
 * (ETS_TG0_T0_LEVEL_INTR_SOURCE); every source has a MAP register at
 * base + 4*source holding the CPU line it feeds.  Lines 1 (WiFi) and 6
 * (permanently disabled) are unusable; we take 2.  TYPE is one bit per
 * line (0 = level, 1 = edge) and level is the reset default.  All routed
 * lines surface in RISC-V terms as mcause 0x80000000 | line - the esp-riscv
 * core reports the routed CPU LINE, not machine-external cause 11 (the tick
 * on line 2 arrives as 0x80000002, verified on the real chip). */
#define TG0_T0_SRC       32u
#define UART0_SRC        21u   /* ETS_UART0_INTR_SOURCE */
#define USJ_SRC          26u   /* ETS_USB_SERIAL_JTAG_INTR_SOURCE */
#define CPU_LINE_TICK    2u
#define CPU_LINE_UART    3u
#define CPU_LINE_USJ     4u
/* NB: third_party/esp32c3-soc/interrupts.h is NOT the C3's table - it is
 * missing the Bluetooth sources and numbers everything six lower (its
 * UART0 reads 15).  The numbers here are the real C3 ones: 21 and 32 are
 * verified on the chip, and 26 follows the same esp-idf file USJ shares
 * that spacing with (UART0, UART1, LEDC, EFUSE, TWAI, USJ). */
#define INT_ENABLE_REG   (INTMTX_BASE + 0x104)
#define INT_PRI_REG(n)   (INTMTX_BASE + 0x114 + 4 * (n))
#define INT_THRESH_REG   (INTMTX_BASE + 0x194)
#define TG_T0_MAP_REG    (INTMTX_BASE + 4 * TG0_T0_SRC)
#define UART0_MAP_REG    (INTMTX_BASE + 4 * UART0_SRC)

/* UART0 receive interrupts.  Polling the 128-byte receive FIFO from the tick
 * is not enough: a tick is 10 ms, which at 115200 baud is 115 bytes, so a
 * burst has only 13 bytes of headroom before the FIFO overruns and drops
 * characters (measured on the board: a 132-byte line arrives intact, 137
 * loses its tail).  Typing by hand never gets close; pasting a command does.
 *
 * Two sources, exactly like the vendor examples: "the FIFO is 30 bytes
 * full" for a stream, and "the receiver went quiet for 10 bit times" so a
 * single hand-typed character is picked up immediately rather than at the
 * next tick. */
#define UART_INT_ENA_REG     (UART0_BASE + 0x0C)
#define UART_INT_CLR_REG     (UART0_BASE + 0x10)
#define UART_CONF1_REG       (UART0_BASE + 0x24)
#define UART_MEM_CONF_REG    (UART0_BASE + 0x60)

#define UART_INT_RXFIFO_FULL  (1u << 0)
#define UART_INT_RXFIFO_TOUT  (1u << 8)
#define UART_CONF1_RX_TOUT_EN (1u << 21)
#define UART_RX_FULL_THRHD    30u
#define UART_RX_TOUT_THRHD    10u

/* ---- receive overrun --------------------------------------------------
 * The console is drained from an interrupt, but the kernel still runs with
 * interrupts off: an exec, a flash write or a long print keeps the FIFO
 * unread for milliseconds, and a 128-byte FIFO at 115200 holds 11 ms.  A
 * burst longer than that (a 24-line paste was enough) overflows it - and
 * this UART does not merely lose the extra bytes, it stops delivering and
 * raises nothing afterwards.  Symptom: the paste runs 20 commands and then
 * the console is deaf while the tick and every task keep running, until
 * the board is reset.  Resetting the receive FIFO is what brings it back.
 * Whatever was still queued is dropped, but at that point it is stale
 * input anyway. */
#define UART_INT_RAW_REG      (UART0_BASE + 0x04)
#define UART_CONF0_REG        (UART0_BASE + 0x20)
#define UART_INT_RXFIFO_OVF   (1u << 4)
#define UART_CONF0_RXFIFO_RST (1u << 17)

unsigned uart_rx_ovf_count;

static void uart_rx_ovf_check(void)
{
    if (!(mmio_read32(UART_INT_RAW_REG) & UART_INT_RXFIFO_OVF))
        return;
    uart_rx_ovf_count++;
    mmio_write32(UART_INT_CLR_REG, UART_INT_RXFIFO_OVF);

    uint32_t conf0 = mmio_read32(UART_CONF0_REG);
    mmio_write32(UART_CONF0_REG, conf0 | UART_CONF0_RXFIFO_RST);
    mmio_write32(UART_CONF0_REG, conf0 & ~UART_CONF0_RXFIFO_RST);
}


static uint32_t tick_reload;
static uint32_t tick_config;

void plat_timer_init(uint32_t hz)
{
    tick_reload = (TIMG_CLK_HZ / TIMG_DIVIDER) / hz;

    /* Register clock gate comes first: bit31 defaults to 0 ("registers
     * can not be read or written by software") while bits 30/29 default
     * to 1.  Writing bit31 ALONE wipes the two default-1 bits and with
     * them the timer's functional clock (verified on the real chip) -
     * all three always go together. */
    mmio_write32(REGCLK_REG,
                 REGCLK_CLK_EN | REGCLK_TIMER_ACTIVE | REGCLK_WDT_ACTIVE);

    /* Route TG0 T0 to CPU line 2 (level type is the reset default of the
     * 1-bit-per-line TYPE register; priority 1), then enable it. */
    mmio_write32(TG_T0_MAP_REG, CPU_LINE_TICK);
    mmio_write32(INT_PRI_REG(CPU_LINE_TICK), 1);
    mmio_write32(INT_THRESH_REG, 1);
    mmio_write32(INT_ENABLE_REG, mmio_read32(INT_ENABLE_REG) | (1u << CPU_LINE_TICK));

    /* Console input on its own line, see the UART_INT_* comment above.
     * The USB-Serial/JTAG takes a third line for the same reason: its RX
     * is one 64-byte USB packet per interrupt, and picking that up at the
     * next tick (64 B/tick = 6.4 KiB/s) was what pinned the file transfer
     * to 1 KiB/s.  Line 4, like the others - level type is the reset
     * default of the TYPE register, priority 1. */
    mmio_write32(UART0_MAP_REG, CPU_LINE_UART);
    mmio_write32(INT_PRI_REG(CPU_LINE_UART), 1);
    mmio_write32(INT_ENABLE_REG, mmio_read32(INT_ENABLE_REG) | (1u << CPU_LINE_UART));

    mmio_write32(INTMTX_BASE + 4 * USJ_SRC, CPU_LINE_USJ);
    mmio_write32(INT_PRI_REG(CPU_LINE_USJ), 1);
    mmio_write32(INT_ENABLE_REG, mmio_read32(INT_ENABLE_REG) | (1u << CPU_LINE_USJ));
    usj_irq_init();               /* route first, then enable the source */

    mmio_write32(UART_CONF1_REG,
                 (mmio_read32(UART_CONF1_REG) & ~0x1FFu) | UART_RX_FULL_THRHD);
    mmio_write32(UART_CONF1_REG,
                 mmio_read32(UART_CONF1_REG) | UART_CONF1_RX_TOUT_EN);
    mmio_write32(UART_MEM_CONF_REG,
                 (mmio_read32(UART_MEM_CONF_REG) & ~(0x3FFu << 16))
                 | (UART_RX_TOUT_THRHD << 16));
    /* Overrun is enabled as well: it is the one condition that leaves the
     * receiver deaf, so it must interrupt rather than wait for the next
     * tick to notice. */
    mmio_write32(UART_INT_ENA_REG,
                 mmio_read32(UART_INT_ENA_REG)
                 | UART_INT_RXFIFO_FULL | UART_INT_RXFIFO_TOUT
                 | UART_INT_RXFIFO_OVF);

    /* Timer: APB / 80 = 1 MHz, counts up from 0 to the reload value
     * (10000 counts = 10 ms), alarm fires, counter reloads.  The divider
     * field holds (divider - 1).  Register readbacks are truthful
     * (ALARMLO reads 0x2710, CONFIG reads back bit-exact) - earlier probe
     * notes claiming otherwise were an artifact of a broken hex-print
     * loop in the probe, not a chip property. */
    mmio_write32(T0LOADLO_REG, 0);
    mmio_write32(T0LOADHI_REG, 0);
    mmio_write32(T0ALARMLO_REG, tick_reload);
    mmio_write32(T0ALARMHI_REG, 0);
    tick_config = T0_EN | T0_INCREASE | T0_AUTORELOAD | T0_ALARM_EN |
                  (((TIMG_DIVIDER - 1) & T0_DIVIDER_M) << T0_DIVIDER_S);
    mmio_write32(T0CONFIG_REG, tick_config);
    mmio_write32(T0LOAD_REG, 1);          /* any write reloads to 0 */

    mmio_write32(INT_ENA_REG, 1u);        /* T0 interrupt enable */

    /* No mie here: the esp-riscv core has no such CSR.  A routed line is
     * enabled by INTMTX alone and gated by mstatus.MIE, which
     * plat_irq_enable() switches on. */
}

static volatile uint64_t g_ticks;

void plat_timer_tick(void)
{
    /* Cheap insurance for the deaf-receiver case: even if the overrun
     * interrupt is masked by the very interrupts-off window that caused
     * it, the next tick brings the console back. */
    uart_rx_ovf_check();
    g_ticks++;
    mmio_write32(INT_CLR_REG, 1u);        /* clear T0 level interrupt */
    /* ALARM_EN self-clears when the alarm fires and AUTORELOAD does not
     * restore it - every tick must re-arm or the heartbeat stops after a
     * single beat (verified on the real chip).  The fully silent handler
     * (no UART access, no readbacks) sustains this indefinitely. */
    mmio_write32(T0CONFIG_REG, tick_config);
}

uint64_t plat_ticks(void)
{
    return g_ticks;
}

uint32_t plat_tick_hz(void)
{
    return TICK_HZ;
}

/* Which routed line is the clock.  Everything else the board wires up is
 * housekeeping and must not run the scheduler's tick path (that would spend
 * a time slice and advance the clock once per received byte). */
int plat_irq_is_tick(uint32_t line)
{
    return line == CPU_LINE_TICK;
}

/* The receive-timeout bit is a latch, not a level: it stays set until it is
 * written back, so the interrupt would re-fire immediately on return and
 * starve every task (symptom: keystrokes echo but nothing executes).  The
 * USJ packet bit behaves the same from the software side - the trap
 * handler drains first (tty_poll), then acks here, so a packet arriving
 * in between re-latches and fires again. */
void plat_irq_service(uint32_t line)
{
    if (line == CPU_LINE_UART) {
        mmio_write32(UART_INT_CLR_REG,
                     UART_INT_RXFIFO_FULL | UART_INT_RXFIFO_TOUT);
        uart_rx_ovf_check();
    }
    if (line == CPU_LINE_USJ)
        usj_irq_service();
}

uint32_t plat_uptime_ms(void)
{
    /* From the clock, not from the tick count: the tick is a scheduling
     * knob, the clock is what the user asked for. */
    return (uint32_t)(plat_now_ns() / 1000000ull);
}

/* Sub-tick part of the clock: TG0 counts at TIMG_CLK_HZ/TIMG_DIVIDER = 1 MHz
 * up to its alarm (10 ms) and then reloads from zero, so the elapsed
 * microseconds inside the current period are simply T0LO/T0HI.
 *
 * The two value registers are snapshots, refreshed only when software asks:
 * T0UPDATE bit 0 latches them, then LO is read first because reading LO
 * latches HI (same protocol esp-idf uses).  Putting the two together with
 * g_ticks gives a microsecond-resolution clock without a second timer.
 *
 * The one race worth handling: the counter can wrap and start counting
 * again before the tick ISR gets around to incrementing g_ticks, which would
 * make the sum go backwards by a full 10 ms.  Time is clamped so it never
 * does. */
uint64_t plat_now_ns(void)
{
    static uint64_t last_ns;

    mmio_write32(T0UPDATE_REG, 1u);
    uint32_t lo = mmio_read32(T0LO_REG);
    uint32_t hi = mmio_read32(T0HI_REG) & 0xFFFFu;
    uint64_t sub_us = ((uint64_t)hi << 32) | lo;

    /* A counter that has not been started yet (or a value larger than the
     * period) must not inflate the clock. */
    if (sub_us > tick_reload)
        sub_us = tick_reload;

    uint64_t ns = ((g_ticks * tick_reload) + sub_us) * 1000ull;

    if (ns < last_ns)
        ns = last_ns;
    last_ns = ns;
    return ns;
}

/* ---- flash block device ---------------------------------------------- */

/* The cache window at 0x3C000000 does not answer guest reads (verified by
 * scripts/esp-c3/drom.S), so the partition is served through the ROM's SPI
 * flash routines instead.  The second-stage bootloader has already probed
 * the chip and loaded this image with them, so no controller setup is
 * needed on our side (verified on the real chip by
 * scripts/esp-c3/flashread.S: flash 0x0 reads back the 0xE9 image header
 * and 0x100000 the littlefs superblock).
 *
 * Addresses are the ROM entry points from esp-idf's esp32c3.rom.ld:
 *   esp_rom_spiflash_read(src_addr, dest, len)   = 0x40000130
 *   esp_rom_spiflash_write(dest_addr, src, len)  = 0x4000012c
 * len is in bytes, dest/src must be word aligned, 0 == success.
 *
 * NOR constraints shape the write path: programming can only clear bits,
 * so a prog is a minimal-change write (words that already hold the wanted
 * value are skipped, a word needing 1->0 conversion is an error).
 *
 * Erase CANNOT use the ROM: all three ROM variants
 * (esp_rom_spiflash_erase_sector 0x40000128, high-level
 * spi_flash_erase_sector 0x40000188, old SPI_sector_erase 0x4000020c)
 * either return 0 without erasing or fail outright - verified on the real
 * chip by a written pattern surviving both calls and a reset
 * (scripts/esp-c3/flasher2.S, flasher3.S, flashcfg.S).  The erase goes
 * through the SPI1 flash controller (SPI_MEM #1, 0x60002000) hardware
 * command bits instead: write 1 to fire, the bit reads back 0 when done,
 * and the controller polls the chip busy flag before finishing
 * (scripts/esp-c3/flashspi.S proved a real erase this way). */

#define ROM_SPIFLASH_WRITE        0x4000012cu
#define ROM_SPIFLASH_READ         0x40000130u

#define SPI1_BASE                 0x60002000u
#define SPI1_CMD_REG              0x00u   /* R/W/SC command bits */
#define SPI1_CMD_FLASH_WREN       (1u << 30)
#define SPI1_CMD_FLASH_SE         (1u << 24)
#define SPI1_ADDR_REG             0x04u   /* address for SE/PP */

typedef int (*rom_spiflash_read_t)(uint32_t addr, uint32_t *dest, int len);
typedef int (*rom_spiflash_write_t)(uint32_t addr, const uint32_t *src, int len);
typedef int (*rom_config_param_t)(uint32_t device_id, uint32_t chip_size,
                                  uint32_t block_size, uint32_t sector_size,
                                  uint32_t page_size, uint32_t status_mask);

#define ROM_SPIFLASH_CONFIG_PARAM 0x40000134u
#define FLASH_CHIP_SIZE   0x400000u   /* 4 MiB */
#define FLASH_BLOCK_SIZE  0x10000u    /* 64 KiB */
#define FLASH_SECTOR_SIZE 0x1000u     /* 4 KiB */
#define FLASH_PAGE_SIZE   0x100u      /* 256 B */
typedef int (*rom_spiflash_erase_t)(uint32_t sector);

#define LFS_LBA_START  PLAT_LFS_LBA           /* 1024 = flash 0x80000 */
#define NOR_SECTOR     4096u
#define LFS_SECTORS    (PLAT_LFS_BLOCKS * 8u) /* 7168 = 3.5 MiB partition */

static rom_spiflash_read_t   rom_flash_read   = (rom_spiflash_read_t)ROM_SPIFLASH_READ;
static rom_spiflash_write_t  rom_flash_write  = (rom_spiflash_write_t)ROM_SPIFLASH_WRITE;
static rom_config_param_t    rom_config_param = (rom_config_param_t)ROM_SPIFLASH_CONFIG_PARAM;

/* The ROM's chip record says 2 MiB after boot, and every ROM flash call
 * range-checks against it (reads past 0x200000 fail: flashsz.S).  Tell
 * the truth: this board has 4 MiB.  Must happen before any ROM flash
 * call; verified by flashcfg.S (config returns 0, 0x3FC000 readable). */
static void rom_flash_set_chip_size(void)
{
    rom_config_param(0, FLASH_CHIP_SIZE, FLASH_BLOCK_SIZE,
                     FLASH_SECTOR_SIZE, FLASH_PAGE_SIZE, 0xFFFFu);
}

/* Fire one SPI1 hardware command bit and wait for it to self-clear. */
static void spi1_command(uint32_t bit)
{
    mmio_write32(SPI1_BASE + SPI1_CMD_REG, bit);
    while (mmio_read32(SPI1_BASE + SPI1_CMD_REG) & bit)
        ;
}

/* Erase one 4 KiB NOR sector by byte offset from the flash base. */
static int spi1_erase_sector_at(uint32_t flash_addr)
{
    mmio_write32(SPI1_BASE + SPI1_ADDR_REG, flash_addr);
    spi1_command(SPI1_CMD_FLASH_WREN);
    spi1_command(SPI1_CMD_FLASH_SE);
    return 0;
}

int plat_block_read(uint32_t lba, uint32_t count, void *buf)
{
    uint8_t *dst = buf;
    uint32_t addr = lba * 512u;   /* lba is GLOBAL (the partition starts at
                                   * PLAT_LFS_LBA = flash 0x100000); callers
                                   * pass PLAT_LFS_LBA + first, so the flash
                                   * address is simply lba * sector_size */

    /* The ROM wants a word-aligned destination.  Sector-sized staging
     * keeps the special case small. */
    for (; count > 0; count--, addr += 512u, dst += 512u) {
        if (((uint32_t)dst & 3u) == 0) {
            if (rom_flash_read(addr, (uint32_t *)dst, 512) != 0)
                return -1;
        } else {
            static uint32_t stage[512 / 4];
            uint32_t i;
            if (rom_flash_read(addr, stage, 512) != 0)
                return -1;
            for (i = 0; i < 512 / 4; i++)
                ((uint32_t *)dst)[i] = stage[i];
        }
    }
    return 0;
}

int plat_block_write(uint32_t lba, uint32_t count, const void *buf)
{
    const uint8_t *src = buf;
    uint32_t addr = lba * 512u;

    /* Write straight through.  This used to read each sector back first and
     * program only the words that differed ("minimal-change prog"), which is
     * safe on NOR but doubles the ROM calls and adds a word-by-word compare
     * in the middle: 44 KiB/s measured, against 88 KiB/s for the same bytes
     * written once.  The only caller that matters is littlefs, which
     * programs blocks it has just erased - every bit it writes is already 1,
     * so there is nothing to check.  A caller that writes over live data
     * still gets NOR semantics (1->0 only); plat_block_erase is the way
     * back. */
    for (; count > 0; count--, addr += 512u, src += 512u) {
        if (((uint32_t)src & 3u) == 0) {
            if (rom_flash_write(addr, (const uint32_t *)src, 512) != 0)
                return -1;
        } else {
            /* The ROM wants word-aligned source data. */
            static uint32_t stage[512 / 4];
            uint32_t i;
            for (i = 0; i < 512 / 4; i++)
                stage[i] = ((const uint32_t *)src)[i];
            if (rom_flash_write(addr, stage, 512) != 0)
                return -1;
        }
    }
    return 0;
}

int plat_block_erase(uint32_t lba, uint32_t count)
{
    uint32_t first = (lba * 512u) / NOR_SECTOR;
    uint32_t last = (lba * 512u + count * 512u - 1) / NOR_SECTOR;
    for (; first <= last; first++)
        if (spi1_erase_sector_at(first * NOR_SECTOR) != 0)
            return -1;
    return 0;
}

uint32_t plat_block_size(void)
{
    return 512;
}

uint32_t plat_block_count(void)
{
    return LFS_SECTORS;
}

/* ---- boot ------------------------------------------------------------ */

void plat_board_early(void)
{
    plat_console_init();
}

void plat_board_init(void)
{
    /* The ROM leaves TG0's watchdog armed: about one second after boot it
     * resets the chip (rst:0x7 TG0WDT_SYS_RST).  Unlock the write
     * protection, then zero the whole config register - the exact probe
     * verified on the real chip.  esptool --after no-reset also leaves
     * the RTC watchdog armed (rst:0x10 RTCWDT_RTC_RST), so it goes too. */
    mmio_write32(WDTWPROTECT_REG, WDT_WKEY);
    mmio_write32(WDTCONFIG0_REG, 0);

    mmio_write32(RTC_WDT_BASE + RTC_WDT_WPROTECT_REG, WDT_WKEY);
    mmio_write32(RTC_WDT_BASE + RTC_WDT_CONFIG0_REG, 0);

    /* And the super watchdog on top of them (own key, own config). */
    mmio_write32(RTC_WDT_BASE + RTC_SWD_WPROTECT_REG, RTC_SWD_WKEY);
    mmio_write32(RTC_WDT_BASE + RTC_SWD_CONF_REG, RTC_SWD_DISABLE);

    /* Fix the ROM's 2 MiB chip-size record before any ROM flash call
     * (the littlefs partition at 0x100000 fits in 2 MiB, but the record
     * should describe the real 4 MiB chip). */
    rom_flash_set_chip_size();
}

/* ---- power ------------------------------------------------------------ */

void plat_shutdown(void)
{
    console_shutdown();
    plat_panic_halt();
}

void plat_reboot(void)
{
    console_shutdown();

    /* console_shutdown() waits for the FIFO to drain, not for the shift
     * register: up to one character (87 us at 115200 baud) is still on the
     * wire, and resetting mid-character makes the host print a broken last
     * line.  A few milliseconds of margin is free next to the reset itself. */
    for (volatile int i = 0; i < 200000; i++)
        ;

    mmio_write32(RTC_WDT_BASE + RTC_CNTL_OPTIONS0_REG, RTC_CNTL_SW_SYS_RST);

    /* Only reached if the reset never happened. */
    plat_panic_halt();
}
