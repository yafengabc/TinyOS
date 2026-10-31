#include "platform.h"
#include "board.h"

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

void plat_console_init(void)
{
    /* See above: the ROM already left the line up. */
}

void plat_console_putc(char c)
{
    /* The FIFO is 128 deep; 100 leaves headroom for our own CRLF pairs. */
    while (uart_tx_count() >= 100)
        ;
    mmio_write32(UART0_BASE + UART_FIFO_REG, (uint8_t)c);
}

int plat_console_getchar(void)
{
    if (uart_rx_count() == 0)
        return -1;
    return mmio_read32(UART0_BASE + UART_FIFO_REG) & 0xFF;
}

/* ---- TIMG0 tick + INTMTX --------------------------------------------- */

/* 80 MHz APB clock, divided down to 1 MHz; 10000 counts per tick = 100 Hz. */
#define TIMG_CLK_HZ    80000000u
#define TIMG_DIVIDER   80u
#define TICK_HZ        100u

#define T0CONFIG_REG   (TIMG0_BASE + 0x00)
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

/* INTMTX (interrupt matrix).  Source 32 is TG0 timer 0
 * (ETS_TG0_T0_LEVEL_INTR_SOURCE); every source has a MAP register at
 * base + 4*source holding the CPU line it feeds.  Lines 1 (WiFi) and 6
 * (permanently disabled) are unusable; we take 2.  TYPE is one bit per
 * line (0 = level, 1 = edge) and level is the reset default.  All routed
 * lines surface in RISC-V terms as mcause 0x80000000 | line - the esp-riscv
 * core reports the routed CPU LINE, not machine-external cause 11 (the tick
 * on line 2 arrives as 0x80000002, verified on the real chip). */
#define TG0_T0_SRC       32u
#define CPU_LINE_TICK    2u
#define INT_ENABLE_REG   (INTMTX_BASE + 0x104)
#define INT_PRI_REG(n)   (INTMTX_BASE + 0x114 + 4 * (n))
#define INT_THRESH_REG   (INTMTX_BASE + 0x194)
#define TG_T0_MAP_REG    (INTMTX_BASE + 4 * TG0_T0_SRC)

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

uint32_t plat_uptime_ms(void)
{
    return (uint32_t)(g_ticks * (1000u / TICK_HZ));
}

/* ---- flash block device ---------------------------------------------- */

/* The chip exposes SPI flash through a cache window at 0x3C000000 (data) /
 * 0x42000000 (instruction).  We probed it on the real chip
 * (scripts/esp-c3/drom.S) and it does NOT work for guest code: the first
 * byte reads as 0x00 where flash 0x0 holds the image magic 0xE9, and the
 * second byte access faults (the probe loops through its own reset window,
 * which is how "one byte per boot" showed up).  Whatever the ROM sets up
 * for its own XIP either does not cover guest reads or is not cache-safe
 * behind them.  Programming the SPI controller directly is the P4 task.
 *
 * Until then there is no block device: ramdisk_init() reports the failed
 * mount and the shell has nothing to load.  plat_block_read returning an
 * error keeps littlefs out of the way instead of feeding it garbage. */

int plat_block_read(uint32_t lba, uint32_t count, void *buf)
{
    (void)lba;
    (void)count;
    (void)buf;
    return -1;
}

int plat_block_write(uint32_t lba, uint32_t count, const void *buf)
{
    (void)lba;
    (void)count;
    (void)buf;
    return -1;
}

int plat_block_erase(uint32_t lba)
{
    (void)lba;
    return -1;
}

uint32_t plat_block_size(void)
{
    return 512;
}

uint32_t plat_block_count(void)
{
    return 0;                             /* no usable device yet */
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
}

/* ---- power ------------------------------------------------------------ */

/* No SiFive test device here.  Reset goes through the RTC_CNTL options
 * byte (P4); until then, halt. */
void plat_shutdown(void)
{
    plat_panic_halt();
}

void plat_reboot(void)
{
    plat_panic_halt();
}
