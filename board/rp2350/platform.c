#include "platform.h"
#include "board.h"
#include "board_internal.h"
#include "arm_memmap.h"
#include "console.h"
#include "usb_cdc.h"
#include "string.h"
#include "cpu.h"

/* Board: Raspberry Pi RP2350 (dual Cortex-M33, ARMv8-M, Secure state).
 *
 * Boot flow: the mask ROM configures XIP itself - there is no RP2040-style
 * 256-byte stage-2 - scans the first 4 KiB of flash for a PICOBIN IMAGE_DEF
 * block, then loads MSP/PC from the ARM vector table at 0x10000000.  Our
 * linker script puts .vectors at flash offset 0 and image_def.S right after
 * it.  The ROM hands over in Secure state (TrustZone is always enabled on
 * the RP2350); we never switch to Non-secure, so the S aliases of every
 * peripheral are the right ones to use.
 *
 * Console: two independent wires, both live at once.  UART0 on GPIO0/1
 * (115200 8N1, for a USB-TTL adapter) AND the on-chip USB device as a
 * CDC-ACM COM port (usb_cdc.c, polled by the tick, no USB interrupt).
 * Either cable alone is enough: plugging the board's own USB port into a PC
 * gives a shell with no extra hardware, and the same output is mirrored to
 * the UART for anyone watching with an adapter.  Input is accepted from both.
 *
 * Clock: the ROM leaves clk_sys on the ~12 MHz ROSC/XOSC path.  Bring-up
 * enables the crystal, pins clk_ref to it, brings PLL_SYS up at 150 MHz
 * (pll_sys_init) and re-pins clk_peri to clk_sys; the frequency counter
 * then *measures* clk_sys against the 12 MHz reference and both the UART
 * baud divisor and the SysTick reload come from that measurement, never
 * from the configured constant.  If the PLL refuses to lock the boot
 * simply continues at the ROM's clock - every divisor still comes out
 * right because it was measured, not assumed.
 *
 * Flash: reads go straight through XIP.  Writes and erases use the RP2350
 * boot ROM's low-level flash API.  Its lookup ABI differs from RP2040: a
 * (code, flags) call through a 16-bit pointer at 0x16 with
 * RT_FLAG_FUNC_ARM_SEC.  Those wrappers live in .data.ramfunc (copied to
 * SRAM by _start) because QSPI goes into serial-command mode mid-call. */

/* ---- boot ROM flash API ------------------------------------------------ */

typedef void *(*rp_rom_lookup_t)(uint32_t code, uint32_t flags);
typedef void (*rp_rom_void_t)(void);
typedef void (*rp_rom_erase_t)(uint32_t addr, size_t count,
                               uint32_t block_size, uint8_t block_cmd);
typedef void (*rp_rom_program_t)(uint32_t addr, const uint8_t *data, size_t count);

#define ROM_FUNC_CONNECT_INTERNAL_FLASH 0x4649u   /* 'I','F' */
#define ROM_FUNC_FLASH_EXIT_XIP        0x5845u   /* 'E','X' */
#define ROM_FUNC_FLASH_ENTER_CMD_XIP   0x5843u   /* 'C','X' */
#define ROM_FUNC_FLASH_FLUSH_CACHE     0x4346u   /* 'F','C' */
#define ROM_FUNC_FLASH_RANGE_ERASE    0x4552u   /* 'R','E' */
#define ROM_FUNC_FLASH_RANGE_PROGRAM   0x5052u   /* 'R','P' */

static void *rp_rom_func(uint32_t code)
{
    rp_rom_lookup_t lookup =
        (rp_rom_lookup_t)(uintptr_t)(*(volatile uint16_t *)BOOTROM_LOOKUP_PTR);
    if (!lookup)
        return 0;
    /* The returned entry is a Thumb address; OR the bit in case the ROM
     * stored it aligned. */
    return (void *)((uintptr_t)lookup(code, RT_FLAG_FUNC_ARM_SEC) | 1u);
}

static rp_rom_void_t    g_rom_connect;
static rp_rom_void_t    g_rom_exit_xip;
static rp_rom_void_t    g_rom_enter_xip;
static rp_rom_void_t    g_rom_flush;
static rp_rom_erase_t   g_rom_erase;
static rp_rom_program_t g_rom_program;
static int              g_rom_ready;

/* Bring-up convenience: a script on COM14 can send one Ctrl-D and have the
   ROM mount BOOTSEL again, instead of a human pressing BOOT + RST before
   every single reflash.  ROM_FUNC_REBOOT is 'R','B' = 0x4252; flags 0x102
   are REBOOT_TYPE_BOOTSEL (0x2) | NO_RETURN_ON_SUCCESS (0x100), the same
   call boot_probe.py uses to get back to the loader after a run. */
typedef void (*rp_rom_reboot_t)(uint32_t flags, uint32_t delay_ms,
                                uint32_t p0, uint32_t p1);
#define ROM_FUNC_REBOOT 0x4252u

/* Ask the ROM to reboot.  RT_FLAG_FUNC_ARM_SEC is the secure-state row of the
 * function table, which is where this kernel runs.  Returns only if the ROM
 * entry point could not be found or the call failed (NO_RETURN_ON_SUCCESS
 * makes success noreturn). */
static void rom_reboot(uint32_t flags)
{
    rp_rom_lookup_t lookup =
        (rp_rom_lookup_t)(uintptr_t)(*(volatile uint16_t *)BOOTROM_LOOKUP_PTR);
    rp_rom_reboot_t reboot;

    if (!lookup)
        return;
    reboot = (rp_rom_reboot_t)
        ((uintptr_t)lookup(ROM_FUNC_REBOOT, RT_FLAG_FUNC_ARM_SEC) | 1u);
    if (!reboot)
        return;
    reboot(flags, 10u, 0u, 0u);
}

static void rom_enter_bootsel(void)
{
    rom_reboot(0x102u);           /* BOOTSEL | NO_RETURN_ON_SUCCESS */
    for (;;)
        ;
}

#define BOOTSEL_MAGIC_CHAR 0x04u      /* Ctrl-D, not a shell editing key */

/* Set the first time anything reads the console.  Until then plat_timer_tick()
 * is allowed to watch UART0 for Ctrl-D, so a kernel that hangs before the
 * shell starts can still be pushed back into the ROM bootloader. */
static volatile int g_console_claimed;

/* Installed as g_panic_poll: once the kernel has died, keep watching UART0
 * for Ctrl-D.  The RX FIFO works without interrupts, which is the same
 * reason plat_shutdown() can poll it, so a crashed image stays recoverable
 * from the host without anyone touching the BOOTSEL button. */
static void panic_poll_uart(void)
{
    if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_RXFE) &&
        (mmio_read32(RP_UART_DR) & 0xffu) == BOOTSEL_MAGIC_CHAR)
        rom_enter_bootsel();
}

/* ---- console: UART0 ----------------------------------------------------- */

#define UART_BAUD 115200u
/* ATK-DNRP2350AM (the board on the bench): the user LED is on GPIO3 and
 * is wired for sink current, i.e. LOW turns it ON.  GPIO25 - the Pico 2
 * convention - has no LED on this board at all. */
#define RP_LED_GPIO 3u

/* plat_board_early() prints its boot line before the USB stack exists, so
 * the console shunts the CDC calls until plat_console_init() has run. */
static int g_usb_ready;

static int g_led_on;

static void led_set(int on)
{
    g_led_on = on ? 1 : 0;
    /* Active-low LED: clearing the output bit turns it ON. */
    mmio_write32(on ? SIO_GPIO_OUT_CLR : SIO_GPIO_OUT_SET, 1u << RP_LED_GPIO);
}

static void led_toggle(void)
{
    led_set(!g_led_on);
}

static void led_init(void)
{
    uint32_t pad = mmio_read32(RP_GPIO_PAD(RP_LED_GPIO));
    /* FUNCSEL first, then de-isolate the pad - the order pico-sdk uses.
     * Leaving ISO set keeps the pin electrically isolated even though every
     * register write succeeds, so the LED never lights. */
    mmio_write32(RP_GPIO_CTRL(RP_LED_GPIO), RP_GPIO_FUNC_SIO);
    mmio_write32(RP_GPIO_PAD(RP_LED_GPIO),
                 (pad | RP_PAD_IE) & ~(RP_PAD_OD | RP_PAD_ISO));
    mmio_write32(SIO_GPIO_OUT_SET, 1u << RP_LED_GPIO);  /* LED off (active low) */
    mmio_write32(SIO_GPIO_OE_SET, 1u << RP_LED_GPIO);
}

static void unreset(uint32_t bits)
{
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) & ~bits);
    for (uint32_t i = 0; i < 1000000u; i++) {
        if ((mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & bits) == bits)
            return;
    }
}

/* clk_ref = XOSC (12 MHz) so FC0 has an honest reference.  clk_sys itself is
 * re-hosted on PLL_SYS by pll_sys_init() below; only clk_peri is pinned to
 * clk_sys here so the UART and the CPU share one rate. */
static void clocks_init(void)
{
    /* ENABLE the crystal first.  The old code only *checked* STABLE, on the
       assumption that the ROM had left XOSC running after a flash boot - it
       has not: XOSC_CTRL_ENABLE resets to DISABLE and the chip comes up on
       ROSC.  The UART and FC0 still look like 12 MHz, which masked it, but
       PLL_USB has no reference and can never reach LOCK. */
    if (!(mmio_read32(XOSC_STATUS) & XOSC_STATUS_STABLE))
        mmio_write32(XOSC_CTRL, XOSC_CTRL_ENABLE_12M);
    for (uint32_t i = 0; i < 1000000u; i++) {
        if (mmio_read32(XOSC_STATUS) & XOSC_STATUS_STABLE)
            break;
    }

    if (mmio_read32(XOSC_STATUS) & XOSC_STATUS_STABLE) {
        mmio_write32(CLOCKS_BASE + CLK_REF_CTRL_OFFSET, CLK_REF_CTRL_SRC_XOSC);
        for (uint32_t i = 0; i < 1000000u; i++) {
            if (mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) & (1u << 2))
                break;
        }
    }
    /* clk_peri = clk_sys, undivided.  AUXSRC 0 is clk_sys on the RP2350. */
    mmio_write32(CLOCKS_BASE + CLK_PERI_DIV_OFFSET, CLK_DIV_1);
    mmio_write32(CLOCKS_BASE + CLK_PERI_CTRL_OFFSET,
                 CLK_PERI_CTRL_ENABLE |
                 (CLK_PERI_CTRL_AUXSRC_CLK_SYS << 5));
}

/* Bring PLL_SYS up at 150 MHz and switch clk_sys onto it.
 *
 * The ROM leaves clk_sys on the ~12 MHz ROSC/XOSC path; at that rate the
 * USB enumeration windows the TinyUSB SIE works inside are uncomfortably
 * tight and every busy-wait bound in the port is stretched to its limit.
 * The pico-sdk default for a 12 MHz crystal is used verbatim: REFDIV 1,
 * FBDIV 125 (12 MHz * 125 = 1500 MHz VCO, inside the 750-1600 window),
 * postdividers 5 and 2 = 150 MHz.
 *
 * Failure is benign: if the PLL never locks the function returns before
 * touching clk_sys and the boot continues on the ROM's clock.  Everything
 * downstream (UART divider, SysTick reload, SPI CPSR) is derived from the
 * value the frequency counter measures afterwards, never from this
 * constant, so both outcomes produce a correct board. */
static void pll_sys_init(void)
{
    const uint32_t refdiv  = 1u;
    const uint32_t fbdiv   = 125u;
    const uint32_t post1   = 5u;
    const uint32_t post2   = 2u;
    uint32_t i;

    /* Force a clean reset/unreset cycle: the ROM may have used this PLL for
     * its own XIP setup and left the dividers anywhere.  Same lesson as
     * PLL_USB in usb_cdc.c - a LOCK bit can be set on a PLL nobody
     * configured, so this boot configures it from scratch regardless. */
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) | RESET_PLL_SYS);
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) & ~RESET_PLL_SYS);
    for (i = 0; i < 1000000u; i++) {
        if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & RESET_PLL_SYS)
            break;
    }

    /* Dividers are programmed while the VCO is powered down, exactly the
     * order pico-sdk's pll_init() uses. */
    PLL_SYS_CS_REG   = refdiv;
    PLL_SYS_FBDIV_REG = fbdiv;
    PLL_SYS_PRIM_REG = (post1 << 16) | (post2 << 12);
    PLL_SYS_PWR_REG &= ~(PLL_SYS_PWR_PD | PLL_SYS_PWR_VCOPD);

    for (i = 0; i < 2000000u; i++) {
        if (PLL_SYS_CS_REG & PLL_SYS_CS_LOCK)
            break;
    }
    if (!(PLL_SYS_CS_REG & PLL_SYS_CS_LOCK))
        return;                     /* no PLL: stay on the ROM's clk_sys   */

    PLL_SYS_PWR_REG &= ~PLL_SYS_PWR_POSTDIVPD;

    /* Switch clk_sys onto the PLL output.  The divider is forced to 1.0
     * first, then SRC moves to the AUX mux (which now selects pll_sys).
     * CLK_SYS_SELECTED must follow before anyone downstream assumes the
     * new rate; until it does the bus is still on the old source. */
    mmio_write32(CLOCKS_BASE + CLK_SYS_DIV_OFFSET, CLK_DIV_1);
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET,
                 CLK_SYS_CTRL_SRC_AUX |
                 (CLK_SYS_CTRL_AUX_PLL_SYS << 5));
    for (i = 0; i < 1000000u; i++) {
        if (mmio_read32(CLOCKS_BASE + CLK_SYS_SELECTED_OFFSET) & (1u << 1))
            break;
    }
}

/* Measure clk_sys in kHz against clk_ref.  Returns 0 when the counter does
 * not settle; the caller then assumes the 12 MHz XOSC. */
/* Measure any FC0 source against the 12 MHz crystal.  A register read only
 * proves that a clock slice is *enabled*; the USB SIE needs clk_usb to be
 * exactly 48 MHz, and a PLL that reports LOCK can still be running at the
 * wrong divider.  Measuring is the only honest check, and an unimplemented
 * or stopped source returns 0 rather than a plausible-looking wrong number. */
static uint32_t measure_clk_src_khz(uint32_t src)
{
    uint32_t spins;

    for (spins = 0; spins < 1000000u; spins++) {
        if (!(mmio_read32(FC0_STATUS) & FC0_STATUS_RUNNING))
            break;
    }
    mmio_write32(FC0_REF_KHZ, 12000u);
    mmio_write32(FC0_MIN_KHZ, 0u);
    mmio_write32(FC0_MAX_KHZ, 0xffffffffu);
    mmio_write32(FC0_INTERVAL, 10u);
    mmio_write32(FC0_SRC, src);                  /* writing starts it */

    for (spins = 0; spins < 1000000u; spins++) {
        if (mmio_read32(FC0_STATUS) & FC0_STATUS_DONE)
            break;
    }
    if (spins == 1000000u)
        return 0;
    return mmio_read32(FC0_RESULT) >> FC0_RESULT_KHZ_S;
}

static uint32_t measure_clk_khz(void)
{
    return measure_clk_src_khz(FC0_SRC_CLK_SYS);
}

static uint32_t g_clk_hz = ARM_SYSTICK_CLK;

static void uart_init(void)
{
    uint32_t div;
    uint32_t pad;

    mmio_write32(RP_UART_CR, 0u);
    mmio_write32(RP_UART_ICR, 0x7ffu);           /* clear all interrupts */

    /* PL011: baud divisor = clk / (16 * baud).  The SDK's
     * (8*clk/baud + 1) trick rounds and separates integer/fraction. */
    div = (8u * g_clk_hz / UART_BAUD) + 1u;
    mmio_write32(RP_UART_IBRD, div >> 7);
    mmio_write32(RP_UART_FBRD, (div & 0x7fu) >> 1);
    mmio_write32(RP_UART_LCR_H, RP_UART_LCR_H_FEN | RP_UART_LCR_H_WLEN);

    /* GPIO0 = UART0 TX, GPIO1 = UART0 RX (FUNCSEL 2 on RP2350).  Each pad
     * must lose its reset-time ISO bit or the CH343 sees no edges at all. */
    mmio_write32(RP_GPIO_CTRL(0), RP_GPIO_FUNC_UART);
    pad = mmio_read32(RP_GPIO_PAD(0));
    mmio_write32(RP_GPIO_PAD(0),
                 (pad | RP_PAD_IE) & ~(RP_PAD_OD | RP_PAD_ISO));
    mmio_write32(RP_GPIO_CTRL(1), RP_GPIO_FUNC_UART);
    pad = mmio_read32(RP_GPIO_PAD(1));
    mmio_write32(RP_GPIO_PAD(1),
                 (pad | RP_PAD_IE) & ~(RP_PAD_OD | RP_PAD_ISO));

    mmio_write32(RP_UART_CR, RP_UART_CR_UARTEN |
                             RP_UART_CR_TXE | RP_UART_CR_RXE);
}

static void early_puts(const char *s)
{
    while (*s)
        plat_console_putc(*s++);
}

static void early_put_hex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 28; i >= 0; i -= 4)
        plat_console_putc(hex[(v >> i) & 0xfu]);
}

void plat_console_init(void)
{
    uart_init();
    early_puts("[P1] uart ready, usb init ...\r\n");
    usb_cdc_console_init(g_clk_hz);
    early_puts("[P2] usb init returned\r\n");

    /* USB must be able to preempt the tick.  SysTick keeps the default
     * priority 0x00 from arch/arm/scs.h, and a handler at the same level
     * cannot interrupt another - but the tick's handler is long enough
     * (diagnostics, USB poll) to blow through the microseconds a
     * SET_ADDRESS status stage allows.  Drop SysTick to 0x20 and leave
     * USBCTRL at 0x00, so enumeration latency no longer depends on what
     * the tick happens to be doing.  SHPR3 packs SysTick in [31:24] and
     * PendSV in [23:16]; only the SysTick byte changes. */
    mmio_write32(ARM_SHPR3,
                 (mmio_read32(ARM_SHPR3) & 0x00FFFFFFu) | (0x20u << 24));

    /* Only let the console shunt characters into the CDC ring when the
       controller actually came up.  With the PLL handshake failed (stage 2)
       there is no endpoint to drain it, so every boot character would just
       fill a 512-byte ring and then be dropped one by one - and every tick
       would poll a device that is still held in reset. */
    g_usb_ready = usb_cdc_hw_ready() ? 1 : 0;

    /* Say hello again on the wire that may have enumerated a moment ago.
     * The UART already saw this line in plat_board_early(); the USB side
     * could not, because the host had not finished SET_CONFIGURATION yet. */
    /* Boot diagnostic.  hw=0 means usb_cdc_init() bailed on the reset/
     * PLL handshake; hw=1/cfg=0 means the PHY is up but the host never
     * finished enumeration.  The measured-clock line sits next to it: the
     * SIE bit-engine needs clk_usb to be exactly 48 MHz to recover bits
     * off the wire, and a PLL that reports LOCK at the wrong divider still
     * detects SE0 but silently drops every packet - printing what FC0
     * actually measured separates those cases in one glance. */
    early_puts("[E] usb hw=");
    early_puts(usb_cdc_hw_ready() ? "1" : "0");
    early_puts(" cfg=");
    early_puts(usb_cdc_configured() ? "1" : "0");
    early_puts(" stage=");
    plat_console_putc((char)('0' + usb_cdc_init_stage()));
    early_puts(" clk_sys_khz=");
    early_put_hex(measure_clk_src_khz(FC0_SRC_CLK_SYS));
    early_puts(" clk_usb_khz=");
    early_put_hex(measure_clk_src_khz(0x0bu));    /* FC0_SRC = CLK_USB */
    early_puts("\r\n");

    if (usb_cdc_configured()) {
        early_puts("\r\nTinyOS/RP2350  usb-cdc=up  clk=0x");
        early_put_hex(g_clk_hz);
        early_puts(" Hz\r\n");
        led_set(0);                 /* enumerated: normal boot          */
    }
    /* USB did not come up: leave the LED lit as a visible "look at UART0"
     * hint rather than pretending the boot succeeded. */

    /* Last step: let the controller interrupt fire.  dcd_init() already
     * registered its handler and armed INTE; this only opens the NVIC gate,
     * so a controller event during the rest of bring-up could not have run
     * before the console was ready. */
    usb_cdc_irq_enable();
}

void plat_console_putc(char c)
{
    uint32_t spins;

    /* The PL011 FIFO must never be able to stall the kernel forever (a
     * board with no adapter still has to reach the USB console). */
    for (spins = 0; spins < 100000u; spins++) {
        if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF))
            break;
    }
    /* Write even if the bounded wait expired: a gated clk_peri can make FR
     * read as "FIFO full" forever, and silently dropping every byte turns
     * a diagnosable boot into a black box.  Worst case is a dropped marker
     * character, not a lost boot log. */
    mmio_write32(RP_UART_DR, (uint8_t)c);
    if (g_usb_ready)
        usb_cdc_putc(c);
}

void plat_console_flush(void)
{
    uint32_t spins;

    for (spins = 0; spins < 100000u; spins++) {
        if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_BUSY))
            break;
    }
    if (g_usb_ready)
        usb_cdc_flush();
}

int plat_console_tx_room(void)
{
    /* A 16-deep FIFO.  Reporting the whole FIFO when not full is enough
     * for the echo path to avoid a blocking write inside the tick.  The CDC
     * ring is larger, so the UART is the limiting side whenever both are
     * connected; usb_cdc_putc() drops rather than block if it ever fills. */
    return (mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF) ? 0 : 16;
}

int plat_console_getchar(void)
{
    int c;

    g_console_claimed = 1;

    /* USB belongs to core 0.  Its interrupt is enabled there, TinyUSB's
     * state machine is not reentrant, and tty_poll() is also called from the
     * block device while a task waits for flash - which can be a task on
     * either core.  Two cores driving one controller is how the board hung
     * solid after a couple of background jobs: CDC stopped answering and
     * UART0 went silent, with the kernel still ticking.
     *
     * Core 1 therefore reads the UART only.  USB bytes still reach a reader
     * on either core, because core 0's tick drains the device into the tty
     * line buffer regardless of which core owns the task. */
    if (plat_cpu_id() == 0) {
        /* Thread context, called on every console poll: the natural place to
         * run the USB state machine when the shell is blocked reading. */
        usb_cdc_poll();

        if (g_usb_ready) {
            c = usb_cdc_getchar();
            if (c >= 0)
                return c;
        }
    }
    if (mmio_read32(RP_UART_FR) & RP_UART_FR_RXFE)
        return -1;
    c = (int)(mmio_read32(RP_UART_DR) & 0xffu);
    /* Handles the whole bring-up loop from a script: one byte on COM14 and
       the board is back in BOOTSEL, no button press.  This is polled from
       the tick, so it works even while the shell is blocked in read(). */
    if (c == BOOTSEL_MAGIC_CHAR)
        rom_enter_bootsel();
    return c;
}

/* ---- SysTick ----------------------------------------------------------- */

static volatile uint64_t g_ticks;
static uint32_t g_reload;

void plat_timer_init(uint32_t hz)
{
    g_reload = g_clk_hz / hz;
    if (g_reload == 0)
        g_reload = 1;
    g_ticks = 0;

    mmio_write32(ARM_SYSTICK_LOAD, g_reload - 1u);
    mmio_write32(ARM_SYSTICK_VAL, 0);
    /* ENABLE | CLKSOURCE (processor clock).  TICKINT stays masked until
     * plat_irq_enable() calls plat_timer_irq_unmask(). */
    mmio_write32(ARM_SYSTICK_CTRL, 5u);
}

uint64_t plat_ticks(void)
{
    return g_ticks;
}

uint32_t plat_tick_hz(void)
{
    return g_clk_hz / g_reload;
}

uint32_t plat_uptime_ms(void)
{
    return (uint32_t)((g_ticks * 1000u) / (uint64_t)plat_tick_hz());
}

uint64_t plat_now_ns(void)
{
    static uint64_t last_ns;
    uint32_t load = g_reload;
    uint32_t val  = mmio_read32(ARM_SYSTICK_VAL) & 0xFFFFFFu;
    uint32_t into = (load > val) ? (load - val) : 0u;
    uint64_t cycles = g_ticks * (uint64_t)load + into;
    uint64_t ns = (cycles / (uint64_t)g_clk_hz) * 1000000000ull
                + ((cycles % (uint64_t)g_clk_hz) * 1000000000ull)
                  / (uint64_t)g_clk_hz;

    if (ns < last_ns)
        ns = last_ns;
    last_ns = ns;
    return ns;
}

void plat_timer_tick(void)
{
    /* Only the boot core counts time.  Every core needs its own tick - it is
     * what preempts whatever that core is running - but the counter they
     * share is global: two cores incrementing it would run the clock at 2x
     * and expire every sleep deadline twice as fast.  Core 0's tick keeps
     * running even when it is idle, so this stays a true wall clock.
     * (Same rule as board/qemu-virt.) */
    int id = plat_cpu_id();

    if (id == 0)
        g_ticks++;              /* SysTick reloads itself */

    if (id != 0)
        return;                 /* the rest of this is core-0 only: USB,
                                 * the LED heartbeat and the BOOTSEL poll all
                                 * belong to the console core */

    /* NO USB work in the tick.  plat_timer_tick() is the SysTick handler, so
     * anything it calls runs on the exception stack with interrupts masked;
     * TinyUSB's task needs the caller's stack and can take a while.  The
     * controller interrupt still handles the SIE events themselves, and the
     * task loop runs from plat_cpu_idle() and the console read path, both of
     * which are ordinary thread context. */

    /* Bring-up trace, one line per state change.  The host may take many
     * seconds to start enumerating and the tick is the only thing that
     * notices, so the log says when the device actually became configured
     * rather than dumping the SIE state every 100 ms.  cfg=1 means
     * SET_CONFIGURATION has been processed and the CDC port exists on the
     * host side; anything else is still in flight. */
    if (g_usb_ready && g_ticks <= 120000u && (g_ticks % 500u) == 0u) {
        static int last_cfg = -1;
        int cfg = usb_cdc_configured();
        if (cfg != last_cfg) {
            last_cfg = cfg;
            early_puts("[U] cfg=");
            plat_console_putc(cfg ? '1' : '0');
            early_puts("\r\n");
        }
    }

    /* A 1 Hz heartbeat (tick is 1000 Hz, so one toggle per second gives a
     * 0.5 Hz square wave).  This is the ONLY boot signal that needs no
     * wiring: solid LED = the kernel died before the timer started, a
     * blinking LED = the kernel is alive and the shell should be up, no
     * matter whether USB CDC or the UART adapter ever enumerates. */
    if ((g_ticks % 1000u) == 0u && !plat_led_owned())
        led_toggle();

    /* Escape hatch for a kernel that hangs before the shell exists.  The
     * shell is normally the only reader of UART0, so a hang before it starts
     * leaves Ctrl-D unread and the board only recoverable with the physical
     * BOOTSEL button.  While nothing has claimed the console yet, the tick
     * peeks at the RX FIFO itself; the moment plat_console_getchar() is first
     * called the claim is recorded and this stops for good, so no keystroke
     * is ever stolen from a running shell. */
    if (!g_console_claimed && (g_ticks % 10u) == 0u &&
        !(mmio_read32(RP_UART_FR) & RP_UART_FR_RXFE)) {
        if ((mmio_read32(RP_UART_DR) & 0xffu) == BOOTSEL_MAGIC_CHAR)
            rom_enter_bootsel();
    }
}

void plat_timer_irq_unmask(void)
{
    uint32_t ctrl = mmio_read32(ARM_SYSTICK_CTRL);
    if (!(ctrl & 2u))
        mmio_write32(ARM_SYSTICK_CTRL, ctrl | 2u);
}

/* A secondary core arms its own SysTick: LOAD/VAL/CTRL are per core, and
 * nothing core 0 wrote applies to us.  TICKINT goes in here (bit 1) rather
 * than through plat_timer_irq_unmask(), which would be happy to see core 0's
 * earlier unmask and skip us. */
void plat_timer_start_cpu(void)
{
    if (g_reload == 0)
        return;
    mmio_write32(ARM_SYSTICK_LOAD, g_reload - 1u);
    mmio_write32(ARM_SYSTICK_VAL, 0);
    mmio_write32(ARM_SYSTICK_CTRL, 7u);     /* ENABLE | TICKINT | CLKSOURCE */
}

/* ---- bringing core 1 up ------------------------------------------------ */

/* Core 1 does not execute our reset vector.  It sits in the boot ROM's
 * wait-for-FIFO loop until core 0 pushes a launch sequence at it, and the
 * ROM then loads VTOR, SP and PC from the three words that follow the
 * handshake.  This is the same protocol pico-sdk's
 * multicore_launch_core1_raw() uses; each word is echoed back, which is how
 * both sides know the other one is alive:
 *
 *     0  0  1  <vector table>  <stack pointer>  <entry>
 *
 * The two leading zeros are not padding: they make the ROM drain whatever
 * stale words a previous run left in its mailbox, and the SDK restarts the
 * whole sequence if an echo comes back wrong.  A stray "0,0,1" with no
 * follow-through is harmless because the ROM goes back to waiting.
 *
 * The stack is core 1's slice of the kernel stack pool and the entry is
 * arm_core1_entry() in arch/arm/start.S, which re-asserts MSP and VTOR and
 * then joins the scheduler. */
extern void arm_core1_entry(void);
extern char __vector_table[];

#define SIO_FIFO_ST (SIO_BASE + 0x050u)
#define SIO_FIFO_WR (SIO_BASE + 0x054u)
#define SIO_FIFO_RD (SIO_BASE + 0x058u)
#define SIO_FIFO_VLD (1u << 0)      /* read: something to pop   */
#define SIO_FIFO_RDY (1u << 1)      /* write: room for another  */

static void sio_fifo_push(uint32_t v)
{
    for (uint32_t spins = 0; spins < 100000u; spins++) {
        if (mmio_read32(SIO_FIFO_ST) & SIO_FIFO_RDY)
            break;
    }
    mmio_write32(SIO_FIFO_WR, v);
    __asm__ volatile("sev" ::: "memory");   /* the other core may be in WFE */
}

static uint32_t sio_fifo_pop(void)
{
    for (uint32_t spins = 0; spins < 100000u; spins++) {
        if (mmio_read32(SIO_FIFO_ST) & SIO_FIFO_VLD)
            return mmio_read32(SIO_FIFO_RD);
        __asm__ volatile("wfe" ::: "memory");
    }
    return 0xFFFFFFFFu;         /* nothing came back: the launch failed */
}

static void sio_fifo_drain(void)
{
    while (mmio_read32(SIO_FIFO_ST) & SIO_FIFO_VLD)
        (void)mmio_read32(SIO_FIFO_RD);
}

int plat_start_cpu(int id)
{
    const uint32_t lead[3] = { 0u, 0u, 1u };
    uint32_t tail[3];
    uint32_t seq = 0;
    uint32_t cmd;
    int restarts = 0;

    if (id != 1)
        return 0;               /* this chip has exactly two cores */

    tail[0] = (uint32_t)(uintptr_t)__vector_table;
    tail[1] = CPU_KSTACK_TOP(1u);
    tail[2] = (uint32_t)(uintptr_t)arm_core1_entry | 1u;  /* Thumb bit */

    for (;;) {
        cmd = (seq < 3) ? lead[seq] : tail[seq - 3];

        if (cmd == 0) {
            /* Drain first: a stale word from an earlier attempt would be
             * echoed as the "response" to this one. */
            sio_fifo_drain();
            __asm__ volatile("sev" ::: "memory");
        }
        sio_fifo_push(cmd);

        if (sio_fifo_pop() != cmd) {
            /* Out of step.  The SDK restarts the whole sequence; only a core
             * that never answers at all is a failure. */
            seq = 0;
            if (++restarts > 4)
                return 0;
            continue;
        }
        if (++seq == 6)
            return 1;           /* core 1 is running arm_core1_entry() */
    }
}

/* ---- block device: NOR flash via the boot ROM -------------------------- */

/* The UF2 packer puts littlefs at flash offset 0x80000, which is
 * PLAT_LFS_LBA * 512.  Erase is 4 KiB aligned (8 littlefs sectors). */

#define RP_FLASH_SECTOR      4096u
#define RP_SECTOR_ERASE_CMD  0x20u

int plat_block_read(uint32_t lba, uint32_t count, void *buf)
{
    const void *src = (const void *)(XIP_BASE + lba * 512u);
    memcpy(buf, src, count * 512u);
    return 0;
}

/* These two must run out of SRAM: rom_flash_exit_xip() puts QSPI into
 * serial-command mode, and the ROM's program/erase routines cannot
 * tolerate an instruction fetch from XIP while they are busy.  The linker
 * script's .data section (AT > FLASH) is copied by _start before kmain. */
#define RAMFUNC __attribute__((section(".data.ramfunc"), noinline))

/* Restore XIP the way pico-sdk's flash_range_erase/program does.
 *
 * The boot ROM leaves a 256-byte XIP-setup routine in bootram: the one its
 * flash scan used, i.e. the fast read mode and read delay it found for this
 * chip.  The SDK copies it to RAM and calls it after flash_flush_cache();
 * calling flash_enter_cmd_xip() instead installs the ROM's own basic mode
 * (03h serial read, CLKDIV=12), whose read delay is counted in clk_sys cycles
 * and is therefore 12.5x too short once clk_sys is at 150 MHz.  The first
 * instruction fetched after the write is then sampled before the flash has
 * driven it, and the core dies on the spot.  At 12 MHz the same mode still
 * works, which is what made the failure look clock-dependent.
 *
 * The +1 is the Thumb bit; pico-sdk notes it is harmless on RISC-V because
 * JALR clears bit 0 of the target. */
#define BOOTRAM_BASE     0x400e0000u
#define BOOT2_SIZE_WORDS 64u

static uint32_t boot2_copyout[BOOT2_SIZE_WORDS];
static int      boot2_copyout_valid;

static RAMFUNC void flash_init_boot2_copyout(void)
{
    const volatile uint32_t *src = (const volatile uint32_t *)BOOTRAM_BASE;

    if (boot2_copyout_valid)
        return;
    for (uint32_t i = 0; i < BOOT2_SIZE_WORDS; i++)
        boot2_copyout[i] = src[i];
    boot2_copyout_valid = 1;
}

static RAMFUNC void flash_enable_xip_via_boot2(void)
{
    ((void (*)(void))(uintptr_t)((uintptr_t)boot2_copyout + 1u))();
}

RAMFUNC int plat_block_erase(uint32_t lba, uint32_t count)
{
    uint32_t addr, end, was_enabled;

    if (!g_rom_ready)
        return -1;

    addr = (lba * 512u) & ~(RP_FLASH_SECTOR - 1u);
    end  = ((lba + count) * 512u + RP_FLASH_SECTOR - 1u)
           & ~(RP_FLASH_SECTOR - 1u);

    __asm__ volatile("mrs %0, primask" : "=r"(was_enabled));
    __asm__ volatile("cpsid i" ::: "memory");
    flash_init_boot2_copyout();
    g_rom_connect();
    g_rom_exit_xip();
    g_rom_erase(addr, end - addr, RP_FLASH_SECTOR, RP_SECTOR_ERASE_CMD);
    g_rom_flush();
    flash_enable_xip_via_boot2();
    if (!(was_enabled & 1u))
        __asm__ volatile("cpsie i" ::: "memory");
    return 0;
}

RAMFUNC int plat_block_write(uint32_t lba, uint32_t count, const void *buf)
{
    uint32_t addr, was_enabled;

    if (!g_rom_ready)
        return -1;

    /* littlefs always writes whole 512-byte sectors: 256-byte flash page
     * alignment and the count multiple are satisfied.  The bounce buffer in
     * ramdisk.c is in SRAM, so the ROM never reads its source from XIP. */
    addr = lba * 512u;

    __asm__ volatile("mrs %0, primask" : "=r"(was_enabled));
    __asm__ volatile("cpsid i" ::: "memory");
    flash_init_boot2_copyout();
    g_rom_connect();
    g_rom_exit_xip();
    g_rom_program(addr, (const uint8_t *)buf, count * 512u);
    g_rom_flush();
    flash_enable_xip_via_boot2();
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
    /* The whole 3.5 MiB littlefs partition is addressable, even though the
     * flashed image only occupies its first 300 KiB; ramdisk_init() grows
     * the filesystem over the rest (see PLAT_LFS_BLOCKS in layout.c). */
    return PLAT_LFS_BLOCKS * 8u;   /* 7168 sectors = 3.5 MiB */
}

/* ---- boot -------------------------------------------------------------- */

void plat_board_early(void)
{
    uint32_t khz;

    /* clk_peri is OFF after reset (CLOCKS_CLK_PERI_CTRL_RESET == 0).  UART0,
     * IO_BANK0 and PADS_BANK0 ALL sit behind it.  Touching any of those
     * registers with the clock gated stalls the AHB fabric FOREVER - it
     * does not return garbage - which is exactly a silent boot with no LED
     * and no byte on the wire.  CLOCKS/RESETS/XOSC themselves need no
     * clk_peri, so the clock must be enabled before anything else here. */
    g_clk_hz = 12000000u;       /* conservative guess until FC0 measures */
    clocks_init();
    pll_sys_init();             /* 150 MHz, or a no-op if the PLL refuses */

    /* RP2350 also leaves most peripherals held in reset.  Same stall rule:
     * release before any GPIO, pad or UART access.  The RP2040 port could
     * get away with touching the pads first because stage-2 had already
     * done both of these steps. */
    unreset(RESET_IO_BANK0 | RESET_PADS_BANK0 | RESET_UART0);

    led_init();
    led_set(1);

    /* Now UART0 is safe.  Emit a progress marker at every boot stage so a
     * single COM14 capture locates a hang without a debugger.  The divider
     * is re-programmed below once clk_sys has actually been measured. */
    uart_init();
    early_puts("\r\n[A] clocks+reset ok\r\n");

    /* From here on a fault is recoverable: the panic loop polls UART0 for
     * Ctrl-D, so a broken build never needs a physical BOOTSEL press. */
    g_panic_poll = panic_poll_uart;

    khz = measure_clk_khz();
    if (khz >= 1000u && khz <= 250000u)
        g_clk_hz = khz * 1000u;
    else
        g_clk_hz = ARM_SYSTICK_CLK;
    uart_init();                    /* re-divide for the real clk_sys */
    early_puts("[B] clk=0x");
    early_put_hex(g_clk_hz);
    early_puts("\r\n");

    /* Repoint exceptions at our vector table in flash.  The ROM already
     * installed it to jump here, but VTOR is ours to own from now on. */
    mmio_write32(ARM_VTOR, XIP_BASE);

    /* The ROM leaves the SAU enabled with eight regions (SAU_CTRL=1,
     * SAU_TYPE=8).  This is a Secure-only image: there is no Non-secure
     * world, no NSC veneer and no secure gateway, so any region that marks
     * a slice of SRAM/flash Non-secure only makes Secure instruction fetches
     * and branches land in an invalid-transition SecureFault.  Disabling the
     * SAU with ALLNS=0 makes the whole address space Secure - the same flat
     * model the RP2040/M0+ port has. */
    mmio_write32(ARM_SAU_CTRL, 0u);
    __asm__ volatile("dsb" ::: "memory");
    __asm__ volatile("isb" ::: "memory");
    early_puts("[C] vtor+sau ok\r\n");

    /* Defensive core-1 park: the ROM normally holds core 1 in its own
     * wait-for-FIFO loop, but a stray multicore launch must not execute
     * our single-core kernel image twice. */
    if (mmio_read32(SIO_CPUID) != 0) {
        for (;;)
            __asm__ volatile("wfe");
    }
    early_puts("[D] cpu0\r\n");

    early_puts("TinyOS/RP2350  clk=0x");
    early_put_hex(g_clk_hz);
    early_puts(" Hz  uart0=115200\r\n");

    /* Bring up the full console (UART re-divide + native USB CDC) the same
     * way every other ARM board does from plat_board_early().  Without this
     * call the CDC controller is never taken out of reset, no 2E8A COM port
     * enumerates, and g_usb_ready stays 0 forever - the kernel would have
     * exactly one wire.  usb_cdc_console_init() waits a bounded ~1.5 s for
     * SET_CONFIGURATION, so a board with no USB cable still boots on UART0.
     * Once the tick is live plat_timer_tick() blinks the LED every second;
     * before that a solid LED means we reached this point. */
    plat_console_init();
}

void plat_board_init(void)
{
    g_rom_connect   = (rp_rom_void_t)rp_rom_func(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    g_rom_exit_xip  = (rp_rom_void_t)rp_rom_func(ROM_FUNC_FLASH_EXIT_XIP);
    g_rom_enter_xip = (rp_rom_void_t)rp_rom_func(ROM_FUNC_FLASH_ENTER_CMD_XIP);
    g_rom_flush     = (rp_rom_void_t)rp_rom_func(ROM_FUNC_FLASH_FLUSH_CACHE);
    g_rom_erase     = (rp_rom_erase_t)rp_rom_func(ROM_FUNC_FLASH_RANGE_ERASE);
    g_rom_program   = (rp_rom_program_t)rp_rom_func(ROM_FUNC_FLASH_RANGE_PROGRAM);
    g_rom_ready = (g_rom_connect && g_rom_exit_xip && g_rom_enter_xip &&
                   g_rom_flush && g_rom_erase && g_rom_program) ? 1 : 0;
}

/* ---- power -------------------------------------------------------------- */

void plat_shutdown(void)
{
    console_shutdown();

    /* console_shutdown() masked every interrupt, so plat_panic_halt()'s
       wfi would never wake and a dead kernel could only be recovered with a
       physical BOOT+RST press.  Keep polling UART0 for Ctrl-D instead: the
       RX FIFO works without interrupts, so a script can always put the ROM
       back in BOOTSEL, even after init has crashed. */
    for (;;) {
        if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_RXFE) &&
            (mmio_read32(RP_UART_DR) & 0xffu) == BOOTSEL_MAGIC_CHAR)
            rom_enter_bootsel();
        for (volatile int i = 0; i < 200; i++)
            ;
    }
}

void plat_reboot(void)
{
    console_shutdown();
    /* Go back through the ROM's own reboot entry with BOOT_TYPE_NORMAL: it
     * re-scans flash for the IMAGE_DEF and boots the same image, exactly the
     * path a power-on takes.  The AIRCR's SYSRESETREQ below is the fallback,
     * not the plan: it leaves the USB controller and the clock tree as they
     * were, and measured on this board the shell came back with a CDC port
     * the host could enumerate but not open, and no UART0 output at all -
     * i.e. it hung. */
    rom_reboot(0x100u);           /* BOOT_TYPE_NORMAL | NO_RETURN_ON_SUCCESS */
    mmio_write32(ARM_AIRCR, ARM_AIRCR_VECTKEY | ARM_AIRCR_SYSRESETREQ);
    plat_panic_halt();
}

uint32_t plat_board_cpu_id(void)
{
    return mmio_read32(SIO_CPUID);
}

const char *plat_cpu_model(void)
{
    return "Cortex-M33 (RP2350)";
}

uint32_t plat_cpu_hz(void)
{
    return g_clk_hz;
}
