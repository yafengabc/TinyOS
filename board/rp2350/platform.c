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

static void rom_stop_other_core(void);   /* defined with the flash helpers */
static void wdt_feed(void);               /* defined with the flash helpers */

static void rom_enter_bootsel(void)
{
    rom_stop_other_core();
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

#if TOS_DIAG
/* What the polling core sees when it looks for a park request.  If the flag
 * is set, the id differs from the owner and the loop still did not run, the
 * problem is inside the loop; if the flag reads as 0, the two cores are not
 * looking at the same variable. */
TOS_NOINIT volatile uint32_t g_park_poll_n;
TOS_NOINIT volatile uint32_t g_park_poll_flag;
TOS_NOINIT volatile uint32_t g_park_poll_id;
TOS_NOINIT volatile uint32_t g_park_poll_owner;

/* 0 idle, 1 initiator waits for the other core, 2 the other core is parked,
 * 3 inside the ROM call, 4 XIP back, 5 unparked. */
TOS_NOINIT volatile uint32_t g_park_phase;

/* Is the other core still alive while the initiator waits for it?  Its SysTick
 * count answers "still scheduling" versus "stopped" - the one thing the
 * initiator cannot tell from its own side. */
volatile uint32_t g_c1_ticks;
#endif

#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
/* Set by core 0 before it takes XIP away for a ROM flash call; the parking
 * block next to plat_block_erase is what acts on it. */
static volatile int g_flash_park;
static volatile int g_flash_parked;
/* The ack is a generation, not a flag.  With a boolean a stale "already
 * parked" left over from a timed-out handshake makes the next writer believe
 * the other core is off XIP when it is not, and the ROM then reads flash
 * under a core that is still fetching from it - a corrupt fetch, dead board,
 * no output. */
static volatile uint32_t g_park_req;
static volatile uint32_t g_park_ack;
/* PARK-FIX: which core called rp_flash_park_other().  The tick and idle
 * checks must never park the initiator: it sits in a thread-context wait
 * loop that is the ONLY code which will ever clear the flag, and once its
 * SysTick catches the flag both cores spin in the RAM loop forever - the
 * first dd on rp2350smp froze the machine exactly that way. */
static volatile int g_flash_park_owner = -1;
/* Set before the ROM bootloader takes the chip: the secondary core has to
 * stop for good.  The ROM does not reset it, so it would still be running
 * the old kernel when the new image starts - two kernels sharing one .bss,
 * and the board dies during the next flash write. */
static volatile int g_core1_stopped;
static void rp_flash_park_loop(void);
#endif

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

#if TOS_DIAG
/* One line of liveness, printed straight to the PL011 (the console ring needs
 * the tick, and the tick is exactly what stops when the machine wedges).
 * Called from the kernel's spin-wait loop, so the last line before the freeze
 * says what each core was doing. */
static void diag_char(char c);        /* defined below */
static void diag_str(const char *s);
TOS_NOINIT static uint32_t g_diag_poll_n;

void plat_diag_poll(void)
{
    char b[12];
    int n;
    uint32_t v;

    /* Only report a wait that is actually stuck.  proc_lock is taken and
     * released hundreds of thousands of times a second by the idle console
     * path, so an unconditional periodic dump floods the wire with noise and
     * buries the one line that matters.  A healthy lock costs a handful of
     * retries; a deadlock runs to millions. */
    if (g_spin_count[0] < 200000u && g_spin_count[1] < 200000u)
        return;
    if ((++g_diag_poll_n & 0xfffffu) != 0u)
        return;

#define DIAG_PUT(s) do { const char *p_ = (s); while (*p_) diag_char(*p_++); } while (0)
#define DIAG_NUM(x) do { uint32_t v_ = (x); if (!v_) { diag_char('0'); } else {         n = 0; while (v_) { b[n++] = (char)('0' + v_ % 10u); v_ /= 10u; }         while (n) diag_char(b[--n]); } } while (0)
#define DIAG_HEX(x) do { uint32_t v_ = (x); diag_str("0x"); for (int i_ = 28; i_ >= 0; i_ -= 4) diag_char("0123456789ABCDEF"[(v_ >> i_) & 0xFu]); } while (0)

    DIAG_PUT("[H] c0="); DIAG_NUM(g_spin_wait[0]);
    DIAG_PUT(" c1="); DIAG_NUM(g_spin_wait[1]);
    DIAG_PUT(" lk0="); DIAG_HEX(g_spin_lock_addr[0]);
    DIAG_PUT(" lk1="); DIAG_HEX(g_spin_lock_addr[1]);
    DIAG_PUT(" sc0="); DIAG_HEX(g_spin_caller[0]);
    DIAG_PUT(" sc1="); DIAG_HEX(g_spin_caller[1]);
    DIAG_PUT(" n0="); DIAG_NUM(g_spin_count[0]);
    DIAG_PUT(" n1="); DIAG_NUM(g_spin_count[1]);
    DIAG_PUT(" isite="); DIAG_NUM(g_idle_site);
    DIAG_PUT(" us0="); DIAG_NUM(g_usb_site[0]);
    DIAG_PUT(" us1="); DIAG_NUM(g_usb_site[1]);
    DIAG_PUT(" mseq="); DIAG_NUM(g_mask_seq[0]);
    DIAG_PUT("/"); DIAG_NUM(g_mask_seq[1]);
    DIAG_PUT(" msite="); DIAG_NUM(g_mask_site[0]);
    DIAG_PUT("/"); DIAG_NUM(g_mask_site[1]);
    DIAG_PUT(" pc0="); DIAG_HEX(g_tick_pc[0]);
    DIAG_PUT(" pc1="); DIAG_HEX(g_tick_pc[1]);
    DIAG_PUT(" pk="); DIAG_NUM(g_park_phase);
    DIAG_PUT(" own="); DIAG_NUM(g_park_poll_owner);
    DIAG_PUT("\r\n");
    v = 0; (void)v;
#undef DIAG_PUT
#undef DIAG_NUM
}
#endif /* TOS_DIAG */

/* ---- watchdog ----------------------------------------------------------
 * A wedge in this kernel is silent: both cores sit in a spin loop, no tick
 * runs, and only a physical reset brings the board back - the ROM's UART
 * bootloader cannot even be entered because the CPU no longer reacts to
 * Ctrl-D.  The watchdog turns that into an automatic restart: the tick feeds
 * it while the system is alive, so a frozen tick stops feeding and the
 * board resets itself.
 *
 * RP2350 puts the watchdog at 0x400d8000.  The RP2040 address (0x40058000)
 * is PLL_USB on this chip, so the constant must not be copied over. */
#define WATCHDOG_BASE     0x400d8000u
#define WATCHDOG_CTRL_OFF 0x00u
#define WATCHDOG_LOAD_OFF 0x04u
#define WATCHDOG_CTRL_ENABLE  (1u << 30)
#define WATCHDOG_CTRL_TRIGGER (1u << 31)
/* ~2 s at the reset-default 1 us tick.  Small enough that a wedge anywhere -
 * including inside the ROM flash call, where the old code disarmed the
 * watchdog entirely and a hang became permanent - turns into a reset instead
 * of a dead board, large enough for one erase+program burst (~50-200 ms). */
#define WATCHDOG_RELOAD   0x001fffffu

static void wdt_feed(void)
{
    mmio_write32(WATCHDOG_BASE + WATCHDOG_LOAD_OFF, WATCHDOG_RELOAD);
}

static void wdt_enable(void)
{
    uint32_t ctrl = mmio_read32(WATCHDOG_BASE + WATCHDOG_CTRL_OFF);

    mmio_write32(WATCHDOG_BASE + WATCHDOG_CTRL_OFF, ctrl | WATCHDOG_CTRL_ENABLE);
    wdt_feed();
}

#if TOS_DIAG
/* Straight to the PL011, bypassing the console ring. */
static void diag_char(char c)
{
    uint32_t spins;

    for (spins = 0; spins < 20000u; spins++)
        if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF))
            break;
    mmio_write32(RP_UART_DR, (uint8_t)c);
}

static void diag_str(const char *s)
{
    while (*s)
        diag_char(*s++);
}

static void diag_u32(uint32_t v)
{
    char b[12];
    int n = 0;

    if (!v) {
        diag_char('0');
        return;
    }
    while (v) {
        b[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n)
        diag_char(b[--n]);
}

/* Bring-up marker for drivers: raw PL011, no console ring, no locks, so it can
 * be printed from inside a USB critical section without re-entering it. */
void plat_boot_mark(const char *s)
{
    diag_str(s);
}

void plat_boot_mark_u32(uint32_t v)
{
    diag_char('=');
    diag_u32(v);
}

void plat_boot_mark_hex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";

    diag_char('=');
    for (int i = 28; i >= 0; i -= 4)
        diag_char(hex[(v >> i) & 0xfu]);
}

/* ---- previous-boot evidence -------------------------------------------- */

/* A watchdog reset does not clear SRAM, and every diagnostic variable above
 * lives in .noinit, so the state that wedged the last boot is still on hand
 * when the next one starts.  Two constants guard against reporting random
 * power-on SRAM as evidence. */
#define DIAG_MAGIC0 0x544f5344u        /* "TOSD" */
#define DIAG_MAGIC1 0x31414744u        /* "1AGD" */

TOS_NOINIT static uint32_t g_diag_magic[2];

/* Live-stall detector state (see live_stall_check). */
TOS_NOINIT static uint32_t g_stall_ctr[2];
TOS_NOINIT static uint32_t g_stall_at[2];
TOS_NOINIT static uint32_t g_stall_said;

static void diag_hex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";

    for (int i = 28; i >= 0; i -= 4)
        diag_char(hex[(v >> i) & 0xfu]);
}

static void diag_kv(const char *k, uint32_t v)
{
    diag_str(k);
    diag_u32(v);
}

static void diag_kx(const char *k, uint32_t v)
{
    diag_str(k);
    diag_hex(v);
}

/* Print what the previous boot died doing, then start this one from a clean
 * slate.  Every field answers one question: maskN says which masked section
 * the core was inside when it stopped ticking (site id plus the caller's
 * return address - resolve it with addr2line), seqN frozen versus advancing
 * says wedged versus merely cycling, tickNpc is where the last tick caught
 * the other core, spinN is the lock address it was waiting on and n0/n1 how
 * many retries that wait had made. */
static void diag_prev_report(void)
{
    int have = (g_diag_magic[0] == DIAG_MAGIC0 &&
                g_diag_magic[1] == DIAG_MAGIC1);

    if (have) {
        diag_str("\r\n[PREV]");
        diag_kv(" m0s=", g_mask_site[0]);
        diag_kx(" m0lr=", g_mask_lr[0]);
        diag_kv(" m0n=", g_mask_seq[0]);
        diag_kv(" m1s=", g_mask_site[1]);
        diag_kx(" m1lr=", g_mask_lr[1]);
        diag_kv(" m1n=", g_mask_seq[1]);
        diag_kx(" pc0=", g_tick_pc[0]);
        diag_kx(" pc1=", g_tick_pc[1]);
        diag_kx(" sp0=", g_spin_lock_addr[0]);
        diag_kx(" sc0=", g_spin_caller[0]);
        diag_kv(" n0=", g_spin_count[0]);
        diag_kx(" sp1=", g_spin_lock_addr[1]);
        diag_kv(" n1=", g_spin_count[1]);
        diag_kv(" is=", g_idle_site);
        diag_kv(" pk=", g_park_phase);
        diag_kv(" pn=", g_park_poll_n);
        diag_kv(" pf=", g_park_poll_flag);
        diag_kv(" po=", g_park_poll_owner);
        diag_kv(" u0=", g_usb_site[0]);
        diag_kv(" u1=", g_usb_site[1]);
        diag_kv(" tc0=", g_tick_ctr[0]);
        diag_kv(" tc1=", g_tick_ctr[1]);
        diag_kv(" mi0=", g_mask_in[0]);
        diag_kv(" mi1=", g_mask_in[1]);
        diag_kv(" si0=", g_spin_in[0]);
        diag_kv(" si1=", g_spin_in[1]);
        diag_kv(" ki0=", g_park_in[0]);
        diag_kv(" ki1=", g_park_in[1]);
        diag_str("\r\n");
    }

    /* Fresh start for everything the report just consumed. */
    for (int i = 0; i < 2; i++) {
        g_mask_site[i] = 0;
        g_mask_lr[i] = 0;
        g_mask_seq[i] = 0;
        g_tick_pc[i] = 0;
        g_tick_lr[i] = 0;
        g_spin_lock_addr[i] = 0;
        g_spin_caller[i] = 0;
        g_spin_count[i] = 0;
        g_spin_wait[i] = 0;
        g_cpu_idle[i] = 0;
        g_usb_site[i] = 0;
    }
    g_idle_site = 0;
    g_park_phase = 0;
    g_park_poll_n = 0;
    g_park_poll_flag = 0;
    g_park_poll_id = 0;
    g_park_poll_owner = 0;
    g_diag_poll_n = 0;

    for (int i = 0; i < 2; i++) {
        g_tick_ctr[i] = 0;
        g_mask_in[i] = 0;
        g_spin_in[i] = 0;
        g_park_in[i] = 0;
        g_stall_ctr[i] = 0;
        g_stall_at[i] = 0;
    }
    g_stall_said = 0;

    /* Armed from here on: a watchdog reset during this boot leaves the
     * evidence below, and the next boot prints it. */
    g_diag_magic[0] = DIAG_MAGIC0;
    g_diag_magic[1] = DIAG_MAGIC1;
}

/* One core notices when the other stops taking ticks, and says what the other
 * was inside at that moment.
 *
 * This is the piece the earlier probes could not provide.  A snapshot taken
 * after the fact only says what a core did *last* - it cannot tell a core
 * wedged inside a critical section from one that left it safely, because
 * every counter looks the same from outside.  A per-core tick heartbeat plus
 * "is it inside a masked section / a spin / a park right now" answers the
 * question while the answer still exists, and the report comes from the
 * *healthy* core over the raw PL011, so it survives a console ring that
 * nothing is draining.
 *
 * The window is 1.2 s: below the watchdog's ~2.1 s so the report lands
 * before the reset, and far above any legitimate gap between ticks (1 ms,
 * with at most a few hundred ms lost to a flash write). */
static void live_stall_check(int id)
{
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    int o = id ^ 1;
    uint32_t now = g_tick_ctr[id];

    if (cpu_count <= 1 || g_tick_ctr[o] == 0u)
        return;                             /* other core not started yet */

    if (g_tick_ctr[o] != g_stall_ctr[o]) {
        g_stall_ctr[o] = g_tick_ctr[o];
        g_stall_at[o]  = now;
        return;
    }
    if (g_stall_said || (uint32_t)(now - g_stall_at[o]) < 1200u)
        return;
    g_stall_said = 1;

    diag_str("\r\n[STALL] live=");
    diag_u32((uint32_t)id);
    diag_kv(" dead=", (uint32_t)o);
    diag_kv(" t=", now);
    diag_kv(" mask=", g_mask_in[o]);
    diag_kv(" site=", g_mask_site[o]);
    diag_kx(" lr=", g_mask_lr[o]);
    diag_kv(" seq=", g_mask_seq[o]);
    diag_kv(" spin=", g_spin_in[o]);
    diag_kx(" lk=", g_spin_lock_addr[o]);
    diag_kx(" from=", g_spin_caller[o]);
    diag_kv(" n=", g_spin_count[o]);
    diag_kv(" park=", g_park_in[o]);
    diag_kv(" ph=", g_park_phase);
    diag_kv(" idle=", g_idle_site);
    diag_kv(" usb=", g_usb_site[o]);
    diag_kx(" pc=", g_tick_pc[o]);
    diag_str("\r\n");
#else
    (void)id;
#endif
}
#endif /* TOS_DIAG */

/* Park the secondary core and leave it there.  The ROM bootloader runs on
 * core 0 only and does not reset the chip when it is done, so without this
 * the secondary would still be running the old kernel when the new image
 * starts up - two kernels on one set of globals, which shows up as a board
 * that dies during its first flash write. */
static void rom_stop_other_core(void)
{
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    if (cpu_count <= 1)
        return;

    g_core1_stopped = 1;
    g_flash_parked = 0;
    g_flash_park_owner = (int)plat_cpu_id();
    uint32_t myreq = ++g_park_req;
    __asm__ volatile("dmb" ::: "memory");
    g_flash_park = 1;
    __asm__ volatile("sev" ::: "memory");

    for (uint32_t i = 0; i < 20000000u; i++) {
        if (g_park_ack == myreq)
            break;
        wdt_feed();
    }
#else
    (void)0;
#endif
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
#if TOS_DIAG
    /* First thing on the wire: what the previous boot died doing, if it died
     * by watchdog.  Nothing has touched SRAM since the reset, so this is the
     * only moment the evidence exists. */
    diag_prev_report();
#endif
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
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    /* Core 0 is about to turn XIP off for a ROM flash call: park here, in
     * RAM, before the next instruction fetch can touch flash.  Never park
     * the core that *initiated* the flash call - it is inside the thread
     * wait that is the only code which will ever clear the flag. */
    if (g_flash_park && (int)plat_cpu_id() != g_flash_park_owner)
        rp_flash_park_loop();
#endif
    /* Only the boot core counts time.  Every core needs its own tick - it is
     * what preempts whatever that core is running - but the counter they
     * share is global: two cores incrementing it would run the clock at 2x
     * and expire every sleep deadline twice as fast.  Core 0's tick keeps
     * running even when it is idle, so this stays a true wall clock.
     * (Same rule as board/qemu-virt.) */
    int id = plat_cpu_id();

#if TOS_DIAG
    /* Before anything else in the tick: if the other core has stopped
     * ticking, say so now, while what killed it is still in memory. */
    live_stall_check(id);
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    if (id == 1)
        g_c1_ticks++;
#endif
#endif

    if (id == 0) {
        g_ticks++;              /* SysTick reloads itself */
        /* Feed the watchdog often enough that a busy tick cannot starve it,
         * but rarely enough not to sit on the bus: 50 ms is far below the
         * count-down and well above the worst-case gap between ticks. */
        if ((g_ticks % 50u) == 0u)
            wdt_feed();
    }

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

#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
/* ---- parking the other core out of XIP ---------------------------------- */

/* While the ROM has QMI in direct mode, any XIP access from the other core is
 * a bus fault, so the secondary core has to be off flash before the first
 * byte of the sequence.  Same scheme as the RP2040 port: a flag, a RAM-resident
 * spin loop, a check in the tick (which preempts whatever core 1 is running)
 * and a check in idle (a core already inside its tick cannot be preempted
 * again, so the tick check alone would never see the flag). */
static RAMFUNC void rp_flash_park_loop(void)
{
#if TOS_DIAG
    g_park_phase = 2;                       /* the other core parked */
    g_park_in[plat_cpu_id() & 1u] = 1;
#endif
    g_flash_parked = 1;
    __asm__ volatile("dsb" ::: "memory");
    /* Busy-spin, not wfe: clearing the flag and signalling is not atomic with
     * entering wfe, and a lost wakeup would park this core for good with
     * interrupts masked.  One erase/program burst is short.
     *
     * The acknowledgement is re-published for each generation, not once on
     * entry.  A core that is still inside this loop when the next request is
     * raised never re-enters it, so a publish-once ack makes that request wait
     * for an acknowledgement the previous one already consumed, and the write
     * then fails after the spin budget - the same defect that produced
     * "dd: write error at block 0 (-1)" on rp2350rvsmp.  Publishing while the
     * flag is still up is safe: the flag is cleared only after XIP is back, so
     * "flag clear" means "you may return to flash". */
    for (volatile uint32_t spins = 0; spins < 200000000u; spins++) {
        if (!g_flash_park)
            break;
        __asm__ volatile("dmb" ::: "memory");
        if (g_park_ack != g_park_req) {
            g_park_ack = g_park_req;        /* published from RAM */
            __asm__ volatile("dmb" ::: "memory");
        }
        /* Parked with interrupts masked, so the tick cannot feed the watchdog
         * either.  A writer that never clears the flag (it timed out and
         * returned) must not turn a recoverable error into a silent reset. */
        if ((spins & 0xfffffu) == 0xfffffu)
            wdt_feed();
    }
#if TOS_DIAG
    g_park_in[plat_cpu_id() & 1u] = 0;
#endif
    if (g_core1_stopped) {
        /* The ROM bootloader is about to run on core 0 and the chip is not
         * reset afterwards, so this core must not come back: the new image
         * would race it for the shared memory.  Stay here, in RAM. */
        for (;;)
            wdt_feed();
    }
    g_flash_parked = 0;
}


void plat_flash_park_poll(void)
{
#if TOS_DIAG
    g_park_poll_n++;
    g_park_poll_flag = (uint32_t)g_flash_park;
    g_park_poll_id = (uint32_t)plat_cpu_id();
    g_park_poll_owner = (uint32_t)g_flash_park_owner;
#endif

    if (g_flash_park && (int)plat_cpu_id() != g_flash_park_owner) {
        /* Mask interrupts BEFORE publishing the acknowledgement.  The park
         * loop starts by telling the writer "I am in RAM now"; if anything
         * can preempt this core after that point, the pre-emption returns to
         * flash - the USB interrupt is priority 0 and its handler lives in
         * XIP - and the writer has already put QMI into serial command mode.
         * The fetch then bus-faults inside an exception whose own handler is
         * also in flash: silent dead core, no ticks, no output, watchdog
         * reset two seconds later.  That is the rp2350smp dd wedge.
         *
         * The tick path already ran masked, which is why only the idle and
         * spin-wait callers ever hit this. */
        uint32_t pm = plat_irq_save();

        rp_flash_park_loop();
        plat_irq_restore(pm);
    }
}

static int rp_flash_park_other(void)
{
    uint32_t primask;
    uint32_t myreq;

    if (cpu_count <= 1)
        return 0;

    /* Mask interrupts for the whole handshake.  With them on, the tick can
     * land in this wait loop: process_tick() may reschedule, the process can
     * migrate to the other core, and the core that started the flash call is
     * no longer the one that will unpark it - the other core then parks as
     * well and nothing ever clears the flag again.  On rp2350smp that froze
     * the machine on an intermittent dd, with no output at all. */
    __asm__ volatile("mrs %0, primask" : "=r"(primask));
    __asm__ volatile("cpsid i" ::: "memory");
#if TOS_DIAG
    {
        uint32_t id = plat_cpu_id() & 1u;

        g_mask_lr[id]   = (uint32_t)__builtin_return_address(0);
        g_mask_site[id] = TOS_MASKSITE_FLASH_PARK;
        g_mask_seq[id]++;
        g_mask_in[id]   = 1;
    }
#endif
    g_flash_parked = 0;
    g_flash_park_owner = (int)plat_cpu_id();
#if TOS_DIAG
    g_park_phase = 1;                       /* request raised */
#endif
    myreq = ++g_park_req;
    __asm__ volatile("dmb" ::: "memory");
    g_flash_park = 1;
    __asm__ volatile("sev" ::: "memory");
    for (uint32_t i = 0; i < 20000000u; i++) {
        /* Only this request's acknowledgement counts.  An ack left over from
         * an earlier, timed-out handshake must not be read as "the other core
         * is parked now". */
        if (g_park_ack == myreq) {
            /* Stay masked: the caller restores the interrupt state once it
             * has put XIP back and unparked the other core. */
            return 0;
        }
        if ((i & 0xfffffu) == 0xfffffu) {
            /* The watchdog is fed from the tick, and this loop runs with
             * interrupts masked - so without this the board would reset
             * itself while waiting for a core that never answers. */
            wdt_feed();
#if TOS_DIAG
            /* The other core is not answering.  Is it still taking ticks?  Is
             * it in the idle poll that also looks for the park flag? */
            diag_char('?');
            diag_str(" c1t=");
            diag_u32(g_c1_ticks);
            diag_str(" idle=");
            diag_u32(g_cpu_idle[1]);
            diag_str(" pk=");
            diag_u32(g_park_phase);
            diag_str(" pn=");
            diag_u32(g_park_poll_n);
            diag_str(" pf=");
            diag_u32(g_park_poll_flag);
            diag_str(" pi=");
            diag_u32(g_park_poll_id);
            diag_str(" po=");
            diag_u32(g_park_poll_owner);
            diag_str(" myid=");
            diag_u32((uint32_t)plat_cpu_id());
            diag_str(" myown=");
            diag_u32(g_park_poll_owner);
            diag_str(" c0pc=");
            diag_u32(g_tick_pc[0]);
            diag_str(" c0lr=");
            diag_u32(g_tick_lr[0]);
            diag_str(" c1pc=");
            diag_u32(g_tick_pc[1]);
            diag_str(" mseq0=");
            diag_u32(g_mask_seq[0]);
            diag_str(" mlr0=");
            diag_u32(g_mask_lr[0]);
            diag_str(" msite0=");
            diag_u32(g_mask_site[0]);
            diag_str(" isite=");
            diag_u32(g_idle_site);
            diag_str(" us0=");
            diag_u32(g_usb_site[0]);
            diag_str(" us1=");
            diag_u32(g_usb_site[1]);
            diag_str("\r\n");
#endif
        }
    }
    g_flash_park = 0;
    g_flash_park_owner = -1;
    __asm__ volatile("sev" ::: "memory");
    if (!(primask & 1u))
        __asm__ volatile("cpsie i" ::: "memory");
    return -1;              /* core 1 never parked: do not kill XIP under it */
}

static void rp_flash_unpark_other(void)
{
    g_flash_park = 0;
    g_flash_park_owner = -1;
    __asm__ volatile("sev" ::: "memory");
}
#endif

RAMFUNC int plat_block_erase(uint32_t lba, uint32_t count)
{
    uint32_t addr, end, was_enabled;

    if (!g_rom_ready)
        return -1;

    addr = (lba * 512u) & ~(RP_FLASH_SECTOR - 1u);
    end  = ((lba + count) * 512u + RP_FLASH_SECTOR - 1u)
           & ~(RP_FLASH_SECTOR - 1u);

    __asm__ volatile("mrs %0, primask" : "=r"(was_enabled));
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    if (rp_flash_park_other() != 0)
        return -1;
#endif
    __asm__ volatile("cpsid i" ::: "memory");
    /* Keep the watchdog armed: it used to be disabled around the ROM call,
     * so a wedge inside the sequence was silent and permanent.  Feed instead -
     * one erase burst is ~50 ms, well inside the reload. */
    wdt_feed();
#if TOS_DIAG
    g_park_phase = 3;                       /* inside the ROM call */
#endif
    flash_init_boot2_copyout();
    g_rom_connect();
    g_rom_exit_xip();
    g_rom_erase(addr, end - addr, RP_FLASH_SECTOR, RP_SECTOR_ERASE_CMD);
    g_rom_flush();
    wdt_feed();
#if TOS_DIAG
    g_park_phase = 4;                       /* XIP is back */
#endif
    flash_enable_xip_via_boot2();
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    rp_flash_unpark_other();
#endif
#if TOS_DIAG
    g_park_phase = 5;                       /* unparked */
#endif
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
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    if (rp_flash_park_other() != 0)
        return -1;
#endif
    __asm__ volatile("cpsid i" ::: "memory");
    wdt_feed();                            /* stay armed; see plat_block_erase */
#if TOS_DIAG
    g_park_phase = 3;                       /* inside the ROM call */
#endif
    flash_init_boot2_copyout();
    g_rom_connect();
    g_rom_exit_xip();
    g_rom_program(addr, (const uint8_t *)buf, count * 512u);
    g_rom_flush();
    wdt_feed();
#if TOS_DIAG
    g_park_phase = 4;                       /* XIP is back */
#endif
    flash_enable_xip_via_boot2();
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    rp_flash_unpark_other();
#endif
#if TOS_DIAG
    g_park_phase = 5;                       /* unparked */
#endif
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

    /* Arm the watchdog now that the clock is final: a kernel that wedges
     * later resets itself instead of waiting for a human with a button. */
    wdt_enable();

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
