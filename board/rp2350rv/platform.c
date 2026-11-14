#include "platform.h"
#include "board.h"
#include "board_internal.h"
#include "rp2350rv_memmap.h"
#include "console.h"
#include "usb_cdc.h"
#include "string.h"

/* Same attribute kernel/cpu.h defines, repeated here rather than pulling that
 * header in: it declares __kernel_stack_top as uint8_t[], which clashes with
 * this file's char[] declaration. */
#define TOS_NOINIT __attribute__((section(".noinit")))

static void wdt_feed(void);        /* defined with the RAM helpers below */
static void wdt_enable(void);
static void early_puts(const char *s);
static void early_put_hex(uint32_t v);

/* Board: Raspberry Pi RP2350, Hazard3 RISC-V cores.
 *
 * Booting is much simpler than on the ARM side: there is no vector table
 * and no stage-2.  The boot ROM finds our PICOBIN IMAGE_DEF at flash offset
 * 0 (board/rp2350rv/image_def.S), jumps to the ENTRY_POINT it carries, and
 * gets out of the way.  The kernel then runs in place from the XIP window
 * at 0x10000000, with .data/.bss in SRAM at 0x20000000.
 *
 * Console: UART0 on GPIO0/1 at 115200 (the CH343 port) and the on-chip USB
 * device as a CDC-ACM COM port, both live at once.  USB is the same TinyUSB
 * device stack the ARM image uses; the only difference is that Hazard3
 * delivers external interrupts through its own CSRs, so this port polls the
 * controller (usb_cdc_poll) instead of taking IRQ 14.  The tight loop in
 * usb_cdc_console_init answers enumeration; after that the 1 ms console
 * path drains it.
 *
 * Timer is the Hazard3 machine timer, whose mtime/mtimecmp live in the SIO
 * block (0xd0000000 + 0x1b0), not in a CLINT and not in the ARM timer.
 *
 * Flash reads go straight through XIP.  Writes and erases use the boot
 * ROM's flash API, looked up through the RISC-V table at 0x7dfa; the ROM
 * wrappers must run from SRAM because QSPI leaves command mode mid-call. */

/* ---- boot ROM ---------------------------------------------------------- */

typedef void *(*rv_rom_lookup_t)(uint32_t code, uint32_t flags);
typedef void (*rv_rom_void_t)(void);
typedef void (*rv_rom_erase_t)(uint32_t addr, size_t count,
                               uint32_t block_size, uint8_t block_cmd);
typedef void (*rv_rom_program_t)(uint32_t addr, const uint8_t *data, size_t count);
typedef void (*rv_rom_reboot_t)(uint32_t flags, uint32_t delay_ms,
                                uint32_t p0, uint32_t p1);

#define ROM_FUNC_CONNECT_INTERNAL_FLASH 0x4649u   /* 'I','F' */
#define ROM_FUNC_FLASH_EXIT_XIP        0x5845u   /* 'E','X' */
#define ROM_FUNC_FLASH_ENTER_CMD_XIP   0x5843u   /* 'C','X' */
#define ROM_FUNC_FLASH_FLUSH_CACHE     0x4346u   /* 'F','C' */
#define ROM_FUNC_FLASH_RANGE_ERASE    0x4552u   /* 'R','E' */
#define ROM_FUNC_FLASH_RANGE_PROGRAM   0x5052u   /* 'R','P' */
#define ROM_FUNC_REBOOT                0x4252u   /* 'R','B' */

/* RISC-V lookup: the entry at 0x7dfa is a 16-bit address of a jump
 * instruction (the code is embedded in the table, unlike ARM where the
 * table holds a pointer), and the second argument is RT_FLAG_FUNC_RISCV. */
static void *rp_rom_func(uint32_t code)
{
    rv_rom_lookup_t lookup =
        (rv_rom_lookup_t)(uintptr_t)(*(volatile uint16_t *)BOOTROM_RV_LOOKUP_PTR);

    if (!lookup)
        return 0;
    return lookup(code, RT_FLAG_FUNC_RISCV);
}

static rv_rom_void_t    g_rom_connect;
static rv_rom_void_t    g_rom_exit_xip;
static rv_rom_void_t    g_rom_enter_xip;
static rv_rom_void_t    g_rom_flush;
static rv_rom_erase_t   g_rom_erase;
static rv_rom_program_t g_rom_program;
static int              g_rom_ready;

/* BOOTSEL on Ctrl-D: the host script sends 0x04 on UART0 and the ROM
 * re-enumerates the drive, so a broken image never needs a button press. */
#define BOOTSEL_MAGIC_CHAR 0x04u

static void rom_enter_bootsel(void)
{
    rv_rom_reboot_t reboot = (rv_rom_reboot_t)rp_rom_func(ROM_FUNC_REBOOT);

    if (reboot)
        reboot(0x102u, 10u, 0u, 0u);   /* BOOTSEL | NO_RETURN_ON_SUCCESS */
    for (;;)
        ;
}

static void rom_reboot_normal(void)
{
    rv_rom_reboot_t reboot = (rv_rom_reboot_t)rp_rom_func(ROM_FUNC_REBOOT);

    if (reboot)
        reboot(0x100u, 10u, 0u, 0u);   /* BOOT_TYPE_NORMAL */
}

/* ---- console: UART0 ---------------------------------------------------- */

#define UART_BAUD 115200u

/* clk_sys as measured after the PLL switch.  The UART divisor and the
 * machine-timer interval both come from here, never from a constant, so a
 * PLL that refuses to lock leaves a correct (if slower) board. */
static uint32_t g_clk_hz = 12000000u;

/* Generation counters for the flash-park handshake.  The boolean above only
 * says "some request is pending"; these say *which* one, so an
 * acknowledgement left over from the previous request can never be mistaken
 * for this one, and a hart that is still parked can acknowledge a new request
 * without leaving the RAM loop it is already inside.
 *
 * Defined outside the SMP conditional because the reset-surviving step
 * marker (flash_step) records them even in a uniprocessor build, where they
 * simply stay zero. */
static volatile uint32_t g_park_req;
static volatile uint32_t g_park_ack;

#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
/* Set by core 0 before it takes XIP away for a ROM flash call; the parking
 * block next to plat_block_erase is what acts on it. */
static volatile int g_flash_park;
static volatile int g_flash_parked;
/* PARK-FIX: which core called rp_flash_park_other().  The tick and idle
 * checks must never park the initiator - it is inside the thread wait that
 * is the only code which will ever clear the flag, and once its tick catches
 * the flag both harts spin in the RAM loop forever. */
static volatile int g_flash_park_owner = -1;
static void rp_flash_park_loop(void);
#endif

/* Set once USB CDC came up and the host finished SET_CONFIGURATION.  Before
 * that, output must not be shunted into a FIFO nobody drains. */
static int g_usb_ready;

/* ---- flash-park / tick diagnostics (TOS_DIAG only) ---------------------
 * A dd write on rp2350rvsmp intermittently came back with -1 ("the other hart
 * never parked") and, once, the other hart panicked with "no runnable
 * process" at the same moment.  Neither symptom says *why* the hart stopped
 * answering, so record the handshake as it happens, keep the record across a
 * reset (.noinit) and print it on the next boot, and shout on the wire the
 * moment a park request times out - while the other hart's state can still be
 * observed rather than inferred.
 *
 *   phase: 1 request raised   2 acked         3 XIP down
 *          4 ROM call returned 5 XIP restored  6 unparked
 *   site:  what a hart was inside when it last ran to a known point
 *          (1 tick handler, 2 idle/spin poll, 3 parked in the RAM loop). */
#if TOS_DIAG
TOS_NOINIT static uint32_t g_diag_magic0;
TOS_NOINIT static uint32_t g_diag_magic1;
TOS_NOINIT static uint32_t g_pk_req;
TOS_NOINIT static uint32_t g_pk_ack;
TOS_NOINIT static uint32_t g_pk_to;
TOS_NOINIT static uint32_t g_pk_phase;
TOS_NOINIT static uint32_t g_pk_last_to;      /* phase frozen at the last timeout */
TOS_NOINIT static uint32_t g_tk[2];           /* ticks actually delivered per hart */
TOS_NOINIT static uint32_t g_site[2];
TOS_NOINIT static uint32_t g_park_enter[2];
TOS_NOINIT static uint32_t g_pk_writer;
TOS_NOINIT static uint32_t g_pk_parker;
TOS_NOINIT static uint32_t g_wr_enter[2];     /* entries into the handshake */
TOS_NOINIT static uint32_t g_wr_live;         /* harts inside it right now */

#define DIAG_MAGIC0 0x544f5344u               /* "TOSD" */
#define DIAG_MAGIC1 0x31414744u               /* "1AGD" */
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

    /* GPIO0 = TX, GPIO1 = RX (FUNCSEL 2).  Clear ISO or the CH343 sees no
     * edges at all even though every register write succeeds. */
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

void plat_console_init(void)
{
    uart_init();
}

void plat_console_putc(char c)
{
    uint32_t spins;

    /* The PL011 FIFO must never stall the kernel forever: a board with no
     * adapter still has to reach the USB console. */
    for (spins = 0; spins < 100000u; spins++) {
        if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF))
            break;
    }
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
    return (mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF) ? 0 : 16;
}

extern void (*g_idle_poll)(void);       /* kernel/process.c: idle hook */

/* Runs the polled USB controller's state machine from the scheduler's idle
 * loop: thread context, interrupts on.  See plat_console_getchar() for why it
 * must not run from the tick. */
static void usb_idle_poll(void)
{
    if (plat_cpu_id() == 0 && g_usb_ready)
        usb_cdc_poll();
}

int plat_console_getchar(void)
{
    int c;

    /* tty_poll() runs with interrupts masked and, on the tick path, inside
     * the timer interrupt.  TinyUSB's state machine does not belong there:
     * a retry loop inside tud_task() then holds the console lock forever and
     * the boot stops mid-line.  The state machine runs from the scheduler's
     * idle loop instead - real thread context, interrupts on (see
     * usb_idle_poll() above).  Only the byte queue is touched here. */
    if (plat_cpu_id() == 0 && g_usb_ready) {
        c = usb_cdc_getchar();
        if (c >= 0)
            return c;
    }

    if (mmio_read32(RP_UART_FR) & RP_UART_FR_RXFE)
        return -1;
    c = (int)(mmio_read32(RP_UART_DR) & 0xffu);
    if (c == BOOTSEL_MAGIC_CHAR)
        rom_enter_bootsel();
    return c;
}

/* Polled from plat_panic_halt(): the RX FIFO still works with interrupts
 * off, so a crashed image can always be pushed back into the ROM loader. */
void plat_board_panic_poll(void)
{
    if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_RXFE) &&
        (mmio_read32(RP_UART_DR) & 0xffu) == BOOTSEL_MAGIC_CHAR)
        rom_enter_bootsel();
}

static void early_puts(const char *s)
{
    while (*s)
        plat_console_putc(*s++);
}

static void early_put_hex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    int i;

    for (i = 28; i >= 0; i -= 4)
        plat_console_putc(hex[(v >> i) & 0xfu]);
}

/* ---- clock tree -------------------------------------------------------- */

static void unreset(uint32_t bits)
{
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) & ~bits);
    for (uint32_t i = 0; i < 1000000u; i++) {
        if ((mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & bits) == bits)
            return;
    }
}

/* clk_ref = XOSC (12 MHz); clk_peri = clk_sys.  The crystal has to be
 * enabled first: XOSC_CTRL_ENABLE resets to DISABLE and a flash boot does
 * not leave it running.
 *
 * The first two writes are the important part on RISC-V: move clk_sys and
 * then clk_ref off their aux muxes before anything resets a PLL.  The boot
 * ROM may have selected PLL_SYS for clk_sys, and resetting the PLL while it
 * is still feeding the core stops the CPU mid-instruction (pico-sdk's
 * runtime_init_clocks does exactly this before pll_init). */
static void clocks_init(void)
{
    /* clk_sys = glitchless source 0 (clk_ref), clk_ref = ROSC. */
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET, 0u);
    for (uint32_t i = 0; i < 1000000u; i++) {
        if (mmio_read32(CLOCKS_BASE + CLK_SYS_SELECTED_OFFSET) & 1u)
            break;
    }
    mmio_write32(CLOCKS_BASE + CLK_REF_CTRL_OFFSET, 0u);
    for (uint32_t i = 0; i < 1000000u; i++) {
        if (mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) & 1u)
            break;
    }

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
    /* TIMER0 is programmed by ticks_init() once it is out of reset. */

    /* clk_peri = clk_sys, undivided.  AUXSRC 0 is clk_sys on the RP2350. */
    mmio_write32(CLOCKS_BASE + CLK_PERI_DIV_OFFSET, CLK_DIV_1);
    mmio_write32(CLOCKS_BASE + CLK_PERI_CTRL_OFFSET,
                 CLK_PERI_CTRL_ENABLE |
                 (CLK_PERI_CTRL_AUXSRC_CLK_SYS << 5));
}

/* The 64-bit TIMER (TIMERAWL, and the RV32 mtime it feeds) only counts while
 * the TICKS block emits its 1 MHz tick: TIMER0_CTRL.ENABLE plus TIMER0_CYCLES
 * = clk_ref/clk_tick - 1.  The ROM sets this up for the clock it booted with,
 * and only its flash boot releases TIMER0 from reset at all, so after a RUN
 * reset the counter is stopped: the scheduler never ticks, the console ring is
 * never drained and every driver delay spins its safety bound.
 *
 * This has to run *after* TIMER0 is out of reset (plat_board_early() does that)
 * and after clk_ref is final.  Programming it while the block is still held in
 * reset is a write into a block that may drop it.
 *
 * A slightly wrong tick rate beats a stopped counter: with no fallback the
 * board behaves exactly like the bug this fixes, so ROSC (no crystal, or a
 * crystal that failed to start) still gets a divisor, from the datasheet's
 * nominal rate, instead of having the timer switched off. */
static void ticks_init(void)
{
    uint32_t khz;

    if (mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) & (1u << 2))
        khz = 12000u;           /* clk_ref = XOSC 12 MHz                  */
    else
        khz = 6500u;            /* clk_ref = ROSC: nominal, drifts, counts */

    mmio_write32(TICKS_BASE + TICKS_TIMER0_CYCLES_OFFSET, khz / 1000u - 1u);
    mmio_write32(TICKS_BASE + TICKS_TIMER0_CTRL_OFFSET, 1u);
}

/* PLL_SYS at 150 MHz.  Failure is benign: the boot continues on clk_ref
 * (12 MHz) and every divisor is re-derived from the measured value.
 *
 * The safety rule is in clocks_init(): clk_sys is already on clk_ref before
 * this runs, so resetting PLL_SYS cannot stop the CPU.  The RISC-V boot ROM
 * is allowed to leave clk_sys on PLL_SYS, which is exactly why the switch
 * away from the aux mux comes first (pico-sdk's runtime_init_clocks order). */
static void pll_sys_init(void)
{
    uint32_t i;

    /* Defensive repeat: if this is ever called on its own, moving clk_sys
     * to the glitchless source is what makes the reset below safe. */
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET, 0u);
    for (i = 0; i < 1000000u; i++) {
        if (mmio_read32(CLOCKS_BASE + CLK_SYS_SELECTED_OFFSET) & 1u)
            break;
    }

    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) | RESET_PLL_SYS);
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) & ~RESET_PLL_SYS);
    for (i = 0; i < 1000000u; i++) {
        if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & RESET_PLL_SYS)
            break;
    }

    PLL_SYS_CS_REG    = 1u;          /* REFDIV  */
    PLL_SYS_FBDIV_REG = 125u;        /* VCO = 1500 MHz */
    PLL_SYS_PRIM_REG  = (5u << 16) | (2u << 12);
    PLL_SYS_PWR_REG  &= ~(PLL_SYS_PWR_PD | PLL_SYS_PWR_VCOPD);

    for (i = 0; i < 2000000u; i++) {
        if (PLL_SYS_CS_REG & PLL_SYS_CS_LOCK)
            break;
    }
    if (!(PLL_SYS_CS_REG & PLL_SYS_CS_LOCK))
        return;

    PLL_SYS_PWR_REG &= ~PLL_SYS_PWR_POSTDIVPD;

    mmio_write32(CLOCKS_BASE + CLK_SYS_DIV_OFFSET, CLK_DIV_1);
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET,
                 CLK_SYS_CTRL_SRC_AUX |
                 (CLK_SYS_CTRL_AUX_PLL_SYS << 5));
    for (i = 0; i < 1000000u; i++) {
        if (mmio_read32(CLOCKS_BASE + CLK_SYS_SELECTED_OFFSET) & (1u << 1))
            break;
    }
}

/* Measure clk_sys in kHz against the 12 MHz crystal.  Returns 0 when the
 * counter does not settle; the caller then keeps its conservative default. */
static uint32_t measure_clk_khz(void)
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
    mmio_write32(FC0_SRC, FC0_SRC_CLK_SYS);

    for (spins = 0; spins < 1000000u; spins++) {
        if (mmio_read32(FC0_STATUS) & FC0_STATUS_DONE)
            break;
    }
    if (spins == 1000000u)
        return 0;
    return mmio_read32(FC0_RESULT) >> FC0_RESULT_KHZ_S;
}

/* ---- LED --------------------------------------------------------------- */

static void led_init(void)
{
    uint32_t pad = mmio_read32(RP_GPIO_PAD(RP_LED_GPIO));

    mmio_write32(RP_GPIO_CTRL(RP_LED_GPIO), RP_GPIO_FUNC_SIO);
    mmio_write32(RP_GPIO_PAD(RP_LED_GPIO),
                 (pad | RP_PAD_IE) & ~(RP_PAD_OD | RP_PAD_ISO));
    mmio_write32(SIO_GPIO_OUT_SET, 1u << RP_LED_GPIO);  /* off (active low) */
    mmio_write32(SIO_GPIO_OE_SET, 1u << RP_LED_GPIO);
}

static void led_toggle(void)
{
    static int on;

    on = !on;
    mmio_write32(on ? SIO_GPIO_OUT_CLR : SIO_GPIO_OUT_SET, 1u << RP_LED_GPIO);
}

/* ---- machine timer ----------------------------------------------------- */

static volatile uint64_t g_ticks;
static uint32_t g_interval;

static uint64_t mtime_read(void)
{
    uint32_t hi1, lo, hi2;

    do {
        hi1 = mmio_read32(SIO_MTIMEH);
        lo  = mmio_read32(SIO_MTIME);
        hi2 = mmio_read32(SIO_MTIMEH);
    } while (hi1 != hi2);
    return ((uint64_t)hi1 << 32) | lo;
}

static void mtime_set_cmp(uint64_t v)
{
    /* The high word is parked at all-ones while the low word changes, so a
     * write cannot look like an already-expired deadline and fire the
     * interrupt in the middle of the update. */
    mmio_write32(SIO_MTIMECMPH, 0xFFFFFFFFu);
    mmio_write32(SIO_MTIMECMP, (uint32_t)v);
    mmio_write32(SIO_MTIMECMPH, (uint32_t)(v >> 32));
}

static void timer_unmask(void)
{
    uint32_t mie;

    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    mie |= (1u << 7);                       /* MTIE: machine timer */
    __asm__ volatile("csrw mie, %0" : : "r"(mie));
}

void plat_timer_init(uint32_t hz)
{
    g_interval = g_clk_hz / hz;
    if (g_interval == 0)
        g_interval = 1;
    g_ticks = 0;

    /* Stop and reset the counter first: the ROM may have left it running
     * at a rate nobody documented. */
    mmio_write32(SIO_MTIME_CTRL, 0u);
    mmio_write32(SIO_MTIME, 0u);
    mmio_write32(SIO_MTIMEH, 0u);
    mtime_set_cmp(0xFFFFFFFFFFFFFFFFull);

    mtime_set_cmp(mtime_read() + g_interval);
    timer_unmask();
    mmio_write32(SIO_MTIME_CTRL,
                 SIO_MTIME_CTRL_EN | SIO_MTIME_CTRL_FULLSPEED);
}

void plat_timer_start_cpu(void)
{
    if (g_interval == 0)
        return;
    mtime_set_cmp(mtime_read() + g_interval);
    timer_unmask();
}

uint64_t plat_ticks(void)
{
    return g_ticks;
}

uint32_t plat_tick_hz(void)
{
    return g_interval ? g_clk_hz / g_interval : 0;
}

uint32_t plat_uptime_ms(void)
{
    /* 32-bit arithmetic only: a 64/32 variable divide would pull in
     * compiler-rt, and the RV32 kernel links with -nostdlib.  At 1 kHz this
     * wraps after 49 days, the same as the ARM port. */
    uint32_t hz = plat_tick_hz();

    return hz ? ((uint32_t)g_ticks * 1000u) / hz : 0;
}

uint64_t plat_now_ns(void)
{
    uint64_t t = mtime_read();
    uint64_t hz = g_clk_hz ? g_clk_hz : 1u;

    /* mtime counts clk_sys cycles with FULLSPEED.  Both divisions are
     * variable, so this is the one place on the port that pulls in
     * kernel/libgcc.c's 64-bit helpers - which exist; the earlier version
     * hardcoded 150 MHz and made clock_ms() run 12.5x fast at the ROM's
     * 12 MHz clock.  Split before scaling so a long uptime cannot overflow. */
    return (t / hz) * 1000000000ull + ((t % hz) * 1000000000ull) / hz;
}

void plat_timer_tick(void)
{
#if TOS_DIAG
    g_tk[(uint32_t)plat_cpu_id() & 1u]++;      /* tick actually delivered */
    g_site[(uint32_t)plat_cpu_id() & 1u] = 1;
#endif
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    /* Core 0 is about to turn XIP off for a ROM flash call: park here, in
     * RAM, before the next instruction fetch can touch flash.  Never park
     * the hart that initiated the flash call. */
    if (g_flash_park && (int)plat_cpu_id() != g_flash_park_owner)
        rp_flash_park_loop();
#endif
    /* Only the boot CPU counts time.  With one hart this is always true; it
     * is written this way so an SMP build would not run the clock at 2x. */
    if (plat_cpu_id() == 0) {
        g_ticks++;
        if ((g_ticks % 50u) == 0u)
            wdt_feed();          /* see the watchdog note above */
        /* Keep the CDC FIFO fed from the ring.  tuD's state machine stays in
         * thread context; this only moves bytes, which is safe here. */
        if (g_usb_ready)
            usb_cdc_tx_kick();
    }
    mtime_set_cmp(mtime_read() + g_interval);

    if (plat_cpu_id() == 0 && (g_ticks % 1000u) == 0u && !plat_led_owned())
        led_toggle();
}

/* Hazard3 reports the machine timer as cause 7, the standard
 * M-mode timer interrupt. */
int plat_irq_is_tick(uint32_t line)
{
    return line == 7u;
}

#if TOS_RV_USB_IRQ
/* mcause 11 = machine external.  Read the XH3IRQ pending array and hand the
 * USB line to the same task the polled path runs, but immediately: the
 * control-transfer timing needs it.  Returning 1 tells the trap handler the
 * line was ours; anything else falls through to its panic. */
int plat_irq_foreign(uint32_t line)
{
    uint32_t pend;

    if (line != 11u)
        return 0;
    __asm__ volatile("csrr %0, 0xbe1" : "=r"(pend));
    if (!(pend & (1u << 14u)))
        return 0;
    usb_cdc_irq_task();
    return 1;
}
#else
/* With TOS_RV_USB_IRQ off, arch/rv32's `extern ... __attribute__((weak))`
 * reference has no definition - and under RV32 medany that does not link
 * to 0 as on ARM, but to a jump-to-self thunk, defeating its own `if (cb)`
 * null check the first time a non-tick line shows up.  Define it for real. */
int plat_irq_foreign(uint32_t line)
{
    (void)line;
    return 0;
}
#endif

void plat_irq_service(uint32_t line)
{
    (void)line;
}

/* ---- block device: NOR flash ------------------------------------------- */

/* Reads go through the XIP window at 0x10000000; the littlefs partition is
 * at flash offset 0x80000 (PLAT_LFS_LBA * 512), exactly like the ARM image. */

int plat_block_read(uint32_t lba, uint32_t count, void *buf)
{
    uint32_t off = lba * 512u;
    uint32_t len = count * 512u;

    if (lba != 0 && off / 512u != lba)                  /* overflow */
        return -1;
    if (off + len < off || off + len > 0x1000000u)      /* past 16 MiB */
        return -1;
    memcpy(buf, (const void *)(XIP_BASE + off), len);
    return 0;
}

/* These two must execute from SRAM: flash_exit_xip puts QSPI into
 * serial-command mode and an instruction fetch from flash then hangs.  The
 * linker script copies .data.ramfunc to SRAM before kmain. */
#define RAMFUNC __attribute__((section(".data.ramfunc"), noinline))

/* ---- watchdog ----------------------------------------------------------
 * A wedge in this kernel is silent: both harts sit in a spin loop, no tick
 * runs, and only a physical reset brings the board back - the ROM's UART
 * bootloader cannot even be entered because the hart no longer reacts to
 * Ctrl-D.  The watchdog turns that into an automatic restart: the tick feeds
 * it while the system is alive, so a frozen tick stops feeding and the board
 * resets itself.
 *
 * RP2350 puts the watchdog at 0x400d8000.  The RP2040 address (0x40058000)
 * is PLL_USB on this chip, so the constant must not be copied over. */
#define WATCHDOG_BASE     0x400d8000u
#define WATCHDOG_CTRL_OFF 0x00u
#define WATCHDOG_LOAD_OFF 0x04u
#define WATCHDOG_CTRL_ENABLE  (1u << 30)
#define WATCHDOG_RELOAD   0x00ffffffu
/* SCRATCH0..7 are cleared only by a power-on reset, so a value written just
 * before a wedge tells the next boot exactly which step it died in - the one
 * piece of evidence a .noinit record cannot give when the reset comes from
 * the watchdog while the flash window has interrupts and the console off. */
#define WATCHDOG_SCRATCH(n) (WATCHDOG_BASE + 0x08u + 4u * (n))

/* Pinned to the RAM section rather than left to the inliner: wdt_feed() is
 * called from inside the park loop and wdt_enable() from inside the flash
 * window, both while XIP is off.  They were inlined into their RAMFUNC
 * callers in every build so far, which is exactly the kind of luck that turns
 * into a dead board the first time the optimiser makes a different choice. */
#define RAMFUNC_WDT __attribute__((section(".data.ramfunc"), noinline))

static RAMFUNC_WDT void wdt_feed(void)
{
    mmio_write32(WATCHDOG_BASE + WATCHDOG_LOAD_OFF, WATCHDOG_RELOAD);
}

static RAMFUNC_WDT void wdt_enable(void)
{
    uint32_t ctrl = mmio_read32(WATCHDOG_BASE + WATCHDOG_CTRL_OFF);

    mmio_write32(WATCHDOG_BASE + WATCHDOG_CTRL_OFF, ctrl | WATCHDOG_CTRL_ENABLE);
    wdt_feed();
}

#if TOS_DIAG
/* ---- flash-park / tick diagnostics ------------------------------------- */
static void diag_kv(const char *k, uint32_t v)
{
    early_puts(k);
    early_put_hex(v);
}

static void diag_prev_report(void)
{
    if (g_diag_magic0 == DIAG_MAGIC0 && g_diag_magic1 == DIAG_MAGIC1) {
        early_puts("\r\n[PREV] phase=");
        early_put_hex(g_pk_last_to);
        diag_kv(" req=", g_pk_req);
        diag_kv(" ack=", g_pk_ack);
        diag_kv(" to=", g_pk_to);
        diag_kv(" t0=", g_tk[0]);
        diag_kv(" t1=", g_tk[1]);
        diag_kv(" s0=", g_site[0]);
        diag_kv(" s1=", g_site[1]);
        diag_kv(" p0=", g_park_enter[0]);
        diag_kv(" p1=", g_park_enter[1]);
        early_puts("\r\n");
    }
    g_pk_req = g_pk_ack = g_pk_to = 0;
    g_pk_phase = 0;
    g_pk_last_to = 0;
    for (int i = 0; i < 2; i++) {
        g_tk[i] = 0;
        g_site[i] = 0;
        g_park_enter[i] = 0;
    }
    g_diag_magic0 = DIAG_MAGIC0;
    g_diag_magic1 = DIAG_MAGIC1;
}

/* Last-step marker that survives any reset (WATCHDOG_SCRATCH).  Must run from
 * RAM: it is called inside the flash window. */
static RAMFUNC void flash_step(uint32_t code)
{
    mmio_write32(WATCHDOG_SCRATCH(0), code);
    mmio_write32(WATCHDOG_SCRATCH(1), g_park_req);
    mmio_write32(WATCHDOG_SCRATCH(2), g_tk[1]);
}
#else
#define flash_step(code) do { } while (0)
#endif

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

extern volatile int cpu_count;

/* While the ROM has QMI in direct mode, any XIP access from the other core is
 * a bus fault, so the secondary core has to be off flash before the first
 * byte of the sequence.  Same scheme as the RP2040 port: a flag, a RAM-resident
 * spin loop, a check in the tick (which preempts whatever core 1 is running)
 * and a check in idle (a core already inside its tick cannot be preempted
 * again, so the tick check alone would never see the flag). */
static RAMFUNC void rp_flash_park_loop(void)
{
#if TOS_DIAG
    {
        uint32_t id = (uint32_t)plat_cpu_id() & 1u;

        g_site[id] = 3;               /* parked */
        g_pk_parker = id;             /* which hart answered */
        g_park_enter[id]++;
        g_pk_phase = 2;               /* acking */
    }
#endif
    g_flash_parked = 1;
    __asm__ volatile("fence rw, rw" ::: "memory");
    /* Busy-spin, not wfe: clearing the flag and signalling is not atomic with
     * entering wfe, and a lost wakeup would park this core for good with
     * interrupts masked.  One erase/program burst is short.
     *
     * The acknowledgement is re-published for every generation, not just on
     * entry.  littlefs issues several block operations back to back, and a
     * hart that is still inside this loop when the next request is raised
     * never re-enters it - with a publish-once ack the writer then waits for
     * an acknowledgement that was already consumed by the previous request
     * and gives up after 20M spins, which is the intermittent
     * "dd: write error at block 0 (-1)" on rp2350rvsmp.  Publishing while the
     * flag is still up is safe: the flag is only cleared after XIP is back,
     * so "flag clear" is exactly "you may return to flash". */
    for (volatile uint32_t spins = 0; spins < 200000000u; spins++) {
        if (!g_flash_park)
            break;
        __asm__ volatile("fence rw, rw" ::: "memory");
        if (g_park_ack != g_park_req) {
            g_park_ack = g_park_req;
            __asm__ volatile("fence rw, rw" ::: "memory");
        }
        if ((spins & 0xfffffu) == 0xfffffu)
            wdt_feed();         /* parked with interrupts masked: tick cannot */
    }
    g_flash_parked = 0;
}

void plat_flash_park_poll(void)
{
#if TOS_DIAG
    g_site[(uint32_t)plat_cpu_id() & 1u] = 2;  /* idle/spin poll */
#endif
    if (g_flash_park && (int)plat_cpu_id() != g_flash_park_owner) {
        /* Mask interrupts BEFORE publishing the acknowledgement.  The park
         * loop tells the writer "I am in RAM now"; anything that preempts
         * this core after that point resumes in flash while the writer has
         * QMI in serial command mode, and the fetch bus-faults inside an
         * exception whose handler is also in flash - a silent dead core.
         * The tick path is already masked; the idle and spin-wait callers
         * reach here with interrupts on. */
        uint32_t irq = plat_irq_save();

        rp_flash_park_loop();
        plat_irq_restore(irq);
    }
}

static int rp_flash_park_other(void)
{
    uint32_t mstatus, myreq;

    if (cpu_count <= 1)
        return 0;

    /* Mask interrupts for the handshake: with them on, the tick can land in
     * this wait loop and reschedule the caller onto the other hart, which
     * then is no longer the one that will unpark - both harts park and
     * nothing clears the flag again. */
    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(mstatus));

    myreq = ++g_park_req;                    /* new generation first */
    g_flash_parked = 0;
    g_flash_park_owner = (int)plat_cpu_id();
#if TOS_DIAG
    g_pk_writer = (uint32_t)plat_cpu_id();
    g_wr_enter[(uint32_t)plat_cpu_id() & 1u]++;
    g_wr_live |= 1u << ((uint32_t)plat_cpu_id() & 1u);
    g_pk_phase = 1;                          /* request raised */
    g_pk_req++;
#endif
    __asm__ volatile("fence rw, rw" ::: "memory");
    g_flash_park = 1;
    __asm__ volatile("slt x0, x0, x1" ::: "memory");
    for (uint32_t i = 0; i < 20000000u; i++) {
        /* Only this request's acknowledgement counts. */
        if (g_park_ack == myreq) {
            /* Stay masked: the caller restores mstatus.MIE after the ROM
             * call and the unpark. */
#if TOS_DIAG
            g_pk_ack++;
            g_wr_live &= ~(1u << ((uint32_t)plat_cpu_id() & 1u));
#endif
            return 0;
        }
        if ((i & 0xfffffu) == 0xfffffu)
            wdt_feed();                      /* this wait runs masked too */
    }
    g_flash_park = 0;
    g_flash_park_owner = -1;
#if TOS_DIAG
    g_pk_to++;                               /* timeout */
    g_wr_live &= ~(1u << ((uint32_t)plat_cpu_id() & 1u));
    g_pk_last_to = g_pk_phase;
    /* The far hart never answered.  Say so on the wire with its last known
     * state, because a caller that sees only -1 cannot tell "slow" from
     * "dead" from "never scheduled". */
    {
        /* Is the far hart merely slow, or has it stopped taking ticks
         * altogether?  Sample its counter, spend ~20 ms, sample again. */
        uint32_t a = g_tk[1];

        for (volatile uint32_t d = 0; d < 300000u; d++)
            ;
        early_puts("\r\n[!pk] t1a=");
        early_put_hex(a);
        early_puts(" t1b=");
        early_put_hex(g_tk[1]);
        early_puts(" we=");
        early_put_hex(g_wr_enter[0] << 16 | g_wr_enter[1]);
        early_puts(" wl=");
        early_put_hex(g_wr_live);
    }
    early_puts("\r\n[!pk] t0=");
    early_put_hex(g_tk[0]);
    early_puts(" t1=");
    early_put_hex(g_tk[1]);
    early_puts(" s0=");
    early_put_hex(g_site[0]);
    early_puts(" s1=");
    early_put_hex(g_site[1]);
    early_puts(" pen0=");
    early_put_hex(g_park_enter[0]);
    early_puts(" pen1=");
    early_put_hex(g_park_enter[1]);
    early_puts(" req=");
    early_put_hex(g_pk_req);
    early_puts(" ack=");
    early_put_hex(g_pk_ack);
    early_puts(" wr=");
    early_put_hex(g_pk_writer);
    early_puts(" park=");
    early_put_hex(g_pk_parker);
    early_puts("\r\n");
#endif
    __asm__ volatile("slt x0, x0, x1" ::: "memory");
    if (mstatus & 8u)
        __asm__ volatile("csrsi mstatus, 8" ::: "memory");
    return -1;              /* core 1 never parked: do not kill XIP under it */
}

static void rp_flash_unpark_other(void)
{
    g_flash_park = 0;
    g_flash_park_owner = -1;
    __asm__ volatile("slt x0, x0, x1" ::: "memory");
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

    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(was_enabled));
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    if (rp_flash_park_other() != 0)
        return -1;
#endif
    /* Feed around the ROM call rather than disarming the watchdog: with it
     * off, a wedge inside the sequence was silent and permanent.  One erase
     * burst is ~50 ms against a 16 s reload. */
    wdt_feed();
    flash_step(0x101);
    flash_init_boot2_copyout();
    g_rom_connect();
#if TOS_DIAG
    g_pk_phase = 3;                          /* XIP going down */
#endif
    flash_step(0x102);
    g_rom_exit_xip();
    flash_step(0x103);
    g_rom_erase(addr, end - addr, RP_FLASH_SECTOR, RP_SECTOR_ERASE_CMD);
    flash_step(0x104);
    g_rom_flush();
#if TOS_DIAG
    g_pk_phase = 4;                          /* ROM call returned */
#endif
    wdt_enable();
    flash_step(0x105);
    flash_enable_xip_via_boot2();
#if TOS_DIAG
    g_pk_phase = 5;                          /* XIP restored */
#endif
    flash_step(0x106);
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    rp_flash_unpark_other();
#endif
#if TOS_DIAG
    g_pk_phase = 6;                          /* unparked */
#endif
    flash_step(0x107);
    if (was_enabled & 8u)
        __asm__ volatile("csrsi mstatus, 8" ::: "memory");
    return 0;
}

RAMFUNC int plat_block_write(uint32_t lba, uint32_t count, const void *buf)
{
    uint32_t addr, was_enabled;

    if (!g_rom_ready)
        return -1;

    addr = lba * 512u;

    __asm__ volatile("csrrci %0, mstatus, 8" : "=r"(was_enabled));
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    if (rp_flash_park_other() != 0)
        return -1;
#endif
    wdt_feed();                 /* stay armed; see the watchdog note above */
    flash_step(0x201);
    flash_init_boot2_copyout();
    g_rom_connect();
#if TOS_DIAG
    g_pk_phase = 3;                          /* XIP going down */
#endif
    flash_step(0x202);
    g_rom_exit_xip();
    flash_step(0x203);
    g_rom_program(addr, (const uint8_t *)buf, count * 512u);
    flash_step(0x204);
    g_rom_flush();
#if TOS_DIAG
    g_pk_phase = 4;                          /* ROM call returned */
#endif
    wdt_enable();
    flash_step(0x205);
    flash_enable_xip_via_boot2();
#if TOS_DIAG
    g_pk_phase = 5;                          /* XIP restored */
#endif
    flash_step(0x206);
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    rp_flash_unpark_other();
#endif
#if TOS_DIAG
    g_pk_phase = 6;                          /* unparked */
#endif
    flash_step(0x207);
    if (was_enabled & 8u)
        __asm__ volatile("csrsi mstatus, 8" ::: "memory");
    return 0;
}

uint32_t plat_block_size(void)
{
    return 512;
}

uint32_t plat_block_count(void)
{
    return PLAT_LFS_BLOCKS * 8u;
}

/* ---- user-mode PMP ----------------------------------------------------- */

/* Hazard3 on the RP2350 supports NAPOT and NA4 but not TOR: pmpcfg A=1 is
 * WARL-mapped to OFF, so arch/rv32/platform.c's TOR regions never match and
 * every user fetch faults at the window base.
 *
 * A window is not rounded up to a power of two any more.  Each of the three
 * pieces the kernel distinguishes - image, data+heap, stack - is covered by
 * as many NAPOT blocks as it takes (largest aligned block first), and the
 * guard hole between image and stack is simply left uncovered, so a store
 * into it faults.  Entry 5 stays the trap vector (arch/rv32 sets it once at
 * boot; this rewrites the same value every switch), and the remaining 15
 * entries are the budget.  A typical window needs 3-5.
 *
 * The budget, not alignment, is now the limit: a window whose pieces fragment
 * into more than 15 blocks cannot be described, so the excess tail is left
 * uncovered and that task faults on it.  That takes a pathological shape -
 * 4 KiB windows on a 384 KiB image - and is loud rather than silent. */
#define PMP_NAPOT 0x18u
#define PMP_R     0x01u
#define PMP_W     0x02u
#define PMP_X     0x04u
#define PMP_ENTRIES 16
#define PMP_VEC_ENTRY 5
#define PMP_PAGE  0x1000u

extern char trap_vectors[];

static uint32_t napot(uint32_t base, uint32_t size)
{
    return (base >> 2) | ((size >> 3) - 1u);
}

/* Cover [base, top) with NAPOT blocks, recording entry numbers in addr[] and
 * cfg[].  Returns the next free entry, or -1 when the budget runs out. */
static int pmp_cover(uint32_t base, uint32_t top, uint32_t perm,
                     uint32_t *addr, uint32_t *cfg, int next)
{
    while (base < top) {
        uint32_t rem = top - base;
        uint32_t size = PMP_PAGE;

        if (next == PMP_VEC_ENTRY)
            next++;
        if (next >= PMP_ENTRIES)
            return -1;
        while (2u * size <= rem && (base & (2u * size - 1u)) == 0)
            size *= 2u;

        addr[next] = napot(base, size);
        cfg[next] = perm | PMP_NAPOT;
        next++;
        base += size;
    }
    return next;
}

int plat_board_task_protect(uint32_t win_base, uint32_t win_end, uint32_t wx,
                            uint32_t brk, uint32_t stack_low,
                            uint32_t stack_top)
{
    uint32_t addr[PMP_ENTRIES], cfg[PMP_ENTRIES];
    uint32_t w[4];
    int next = 0;

    for (int i = 0; i < PMP_ENTRIES; i++) {
        addr[i] = 0;
        cfg[i] = 0;
    }
    (void)win_end;

    next = pmp_cover(win_base, wx, PMP_R | PMP_X, addr, cfg, next);
    if (next >= 0)
        next = pmp_cover(wx, brk, PMP_R | PMP_W, addr, cfg, next);
    if (next >= 0)
        next = pmp_cover(stack_low, stack_top, PMP_R | PMP_W, addr, cfg, next);
    if (next < 0)
        early_puts("\r\n[pmp] window needs more NAPOT blocks than the budget\r\n");

    addr[PMP_VEC_ENTRY] = napot((uint32_t)(uintptr_t)trap_vectors, 256u);
    cfg[PMP_VEC_ENTRY] = PMP_NAPOT | PMP_R | PMP_X;

    __asm__ volatile("csrw pmpaddr0, %0" : : "r"(addr[0]));
    __asm__ volatile("csrw pmpaddr1, %0" : : "r"(addr[1]));
    __asm__ volatile("csrw pmpaddr2, %0" : : "r"(addr[2]));
    __asm__ volatile("csrw pmpaddr3, %0" : : "r"(addr[3]));
    __asm__ volatile("csrw pmpaddr4, %0" : : "r"(addr[4]));
    __asm__ volatile("csrw pmpaddr5, %0" : : "r"(addr[5]));
    __asm__ volatile("csrw pmpaddr6, %0" : : "r"(addr[6]));
    __asm__ volatile("csrw pmpaddr7, %0" : : "r"(addr[7]));
    __asm__ volatile("csrw pmpaddr8, %0" : : "r"(addr[8]));
    __asm__ volatile("csrw pmpaddr9, %0" : : "r"(addr[9]));
    __asm__ volatile("csrw pmpaddr10, %0" : : "r"(addr[10]));
    __asm__ volatile("csrw pmpaddr11, %0" : : "r"(addr[11]));
    __asm__ volatile("csrw pmpaddr12, %0" : : "r"(addr[12]));
    __asm__ volatile("csrw pmpaddr13, %0" : : "r"(addr[13]));
    __asm__ volatile("csrw pmpaddr14, %0" : : "r"(addr[14]));
    __asm__ volatile("csrw pmpaddr15, %0" : : "r"(addr[15]));

    for (int g = 0; g < 4; g++)
        w[g] = cfg[4 * g] | (cfg[4 * g + 1] << 8) |
               (cfg[4 * g + 2] << 16) | (cfg[4 * g + 3] << 24);
    __asm__ volatile("csrw pmpcfg0, %0" : : "r"(w[0]));
    __asm__ volatile("csrw pmpcfg1, %0" : : "r"(w[1]));
    __asm__ volatile("csrw pmpcfg2, %0" : : "r"(w[2]));
    __asm__ volatile("csrw pmpcfg3, %0" : : "r"(w[3]));
    return 1;
}

/* Chunked NAPOT needs no alignment from the kernel, and a stack that grows by
 * a 4 KiB step is just one more block, so the following two answers change
 * together with the layout: 4 KiB windows get their one-step reserve, and
 * process_stack_grow() stops demanding that stack_top divide the new size. */
uint32_t plat_window_grow_reserve(void)
{
    return PLAT_STACK_BYTES;
}

int plat_window_needs_stack_alignment(void)
{
    return 0;
}

/* ---- bringing core 1 up ------------------------------------------------ */

/* Core 1 does not run our reset vector: the RP2350 boot ROM holds it in a
 * wait-for-FIFO loop until core 0 pushes the launch sequence
 *
 *     0  0  1  <mtvec>  <sp>  <_start>
 *
 * and echoes every word back.  This is pico-sdk's
 * multicore_launch_core1_raw() protocol for RISC-V.  _start re-reads
 * mhartid, takes .Lsecondary, sees cpu_release and joins the scheduler on
 * its own kernel stack and trap table.  The vector passed here is
 * overwritten there, but the ROM wants a value, so hand it the same
 * direct-mode base core 0 uses. */

#define SIO_FIFO_ST  (SIO_BASE + 0x050u)
#define SIO_FIFO_WR  (SIO_BASE + 0x054u)
#define SIO_FIFO_RD  (SIO_BASE + 0x058u)
#define SIO_FIFO_VLD (1u << 0)
#define SIO_FIFO_RDY (1u << 1)

extern void _start(void);
extern char trap_vectors[];
extern char __kernel_stack_top[];

static void sio_fifo_push(uint32_t v)
{
    for (uint32_t spins = 0; spins < 100000u; spins++) {
        if (mmio_read32(SIO_FIFO_ST) & SIO_FIFO_RDY)
            break;
    }
    mmio_write32(SIO_FIFO_WR, v);

    /* h3.unblock: the RISC-V equivalent of ARM's SEV.  Core 1 waits in
     * h3.block (msleep SLEEPONBLOCK|POWERDOWN); a plain fence would not
     * wake it.  "slt x0, x0, x1" is the custom encoding and is an
     * architectural no-op on a core without the XH3POWER extension. */
    __asm__ volatile("slt x0, x0, x1" ::: "memory");
}

static uint32_t sio_fifo_pop(void)
{
    for (uint32_t spins = 0; spins < 100000u; spins++) {
        if (mmio_read32(SIO_FIFO_ST) & SIO_FIFO_VLD)
            return mmio_read32(SIO_FIFO_RD);
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
    int restarts = 0;

    if (id != 1)
        return 0;               /* this chip has exactly two cores */

    tail[0] = (uint32_t)(uintptr_t)trap_vectors;
    tail[1] = (uint32_t)(uintptr_t)__kernel_stack_top;
    tail[2] = (uint32_t)(uintptr_t)_start;

    for (;;) {
        uint32_t cmd = (seq < 3) ? lead[seq] : tail[seq - 3];

        if (cmd == 0) {
            /* Drain first: a stale word from an earlier attempt would be
             * echoed as the "response" to this one. */
            sio_fifo_drain();
            __asm__ volatile("fence" ::: "memory");
        }
        sio_fifo_push(cmd);

        if (sio_fifo_pop() != cmd) {
            /* Out of step.  Restart the sequence; only a core that never
             * answers at all is a failure. */
            seq = 0;
            if (++restarts > 4)
                return 0;
            continue;
        }
        if (++seq == 6)
            return 1;           /* core 1 is in _start -> .Lsecondary */
    }
}

/* ---- boot -------------------------------------------------------------- */

void plat_board_early(void)
{
    uint32_t khz;

    /* clk_peri is OFF after reset, and UART0, IO_BANK0 and PADS_BANK0 all
     * sit behind it.  Touching one of those with the clock gated stalls the
     * bus forever, so the clock tree is set up before anything else.
     *
     * clocks_init() leaves clk_sys on clk_ref (12 MHz) on purpose: only
     * after that is it safe to reset PLL_SYS.  The [A] marker is printed
     * before that reset, so a hang in the PLL step is visible on the wire
     * instead of looking like a dead board. */
    g_clk_hz = 12000000u;
    clocks_init();
    wdt_enable();               /* a wedge later resets the board by itself */

    /* TIMER0 is the 64-bit microsecond counter behind TIMERAWL *and* behind
     * the RV32 mtime/mtimecmp pair, and it comes out of reset held in reset.
     * The ROM only deasserts it as part of its flash boot, so after a plain
     * RUN reset it stays frozen: every delay in the panel driver spins its
     * safety bound, mtime never advances, the scheduler never ticks and the
     * console ring is never drained - the boot then hangs after the banner. */
    unreset(RESET_IO_BANK0 | RESET_PADS_BANK0 | RESET_UART0 |
            RESET_TIMER0 | RESET_TIMER1);

    /* Only now is the tick generator writable and its clock final. */
    ticks_init();

    led_init();

    uart_init();
    early_puts("\r\n[A] pre-pll ok\r\n");
    /* Let the shift register finish before clk_peri changes: the last bytes
     * of that line would otherwise be clocked out at 150 MHz with the
     * 12 MHz divisor and arrive as garbage. */
    plat_console_flush();

    pll_sys_init();

    khz = measure_clk_khz();
    if (khz >= 1000u && khz <= 250000u)
        g_clk_hz = khz * 1000u;
    uart_init();                    /* re-divide for the real clk_sys */

    early_puts("[B] clk=0x");
    early_put_hex(g_clk_hz);
    early_puts(" Hz\r\n");
#if TOS_DIAG
    diag_prev_report();             /* what the last boot died doing */
    {
        early_puts("[SCR] ");
        for (uint32_t i = 0; i < 4u; i++) {
            early_put_hex(mmio_read32(WATCHDOG_SCRATCH(i)));
            early_puts(" ");
        }
        early_puts("\r\n");
    }
#endif

    /* USB CDC shares the console with UART0.  The wait inside is bounded
     * (~1.5 s scaled by the measured clock), so a board with no USB cable
     * still boots on UART; a cable gets a native COM port as well. */
    usb_cdc_console_init(g_clk_hz);
    g_usb_ready = usb_cdc_hw_ready() ? 1 : 0;

    /* Open the external-interrupt gate (XH3IRQ line 14) now that the
     * controller is up.  Empty when built for polling.  mstatus.MIE is still
     * closed here, so nothing can fire until the scheduler starts. */
    usb_cdc_irq_enable();

    early_puts("[U] usb hw=");
    early_puts(g_usb_ready ? "1" : "0");
    early_puts(" cfg=");
    early_puts(usb_cdc_configured() ? "1" : "0");
    early_puts(" stage=");
    plat_console_putc((char)('0' + usb_cdc_init_stage()));
    early_puts("\r\n");

    early_puts("TinyOS/RP2350  rv32  uart0=115200  usb-cdc=");
    early_puts(g_usb_ready ? "up" : "down");
    early_puts("\r\n");
}

void plat_board_init(void)
{
    g_rom_connect   = (rv_rom_void_t)rp_rom_func(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    g_rom_exit_xip  = (rv_rom_void_t)rp_rom_func(ROM_FUNC_FLASH_EXIT_XIP);
    g_rom_enter_xip = (rv_rom_void_t)rp_rom_func(ROM_FUNC_FLASH_ENTER_CMD_XIP);
    g_rom_flush     = (rv_rom_void_t)rp_rom_func(ROM_FUNC_FLASH_FLUSH_CACHE);
    g_rom_erase     = (rv_rom_erase_t)rp_rom_func(ROM_FUNC_FLASH_RANGE_ERASE);
    g_rom_program   = (rv_rom_program_t)rp_rom_func(ROM_FUNC_FLASH_RANGE_PROGRAM);
    g_rom_ready = (g_rom_connect && g_rom_exit_xip && g_rom_enter_xip &&
                   g_rom_flush && g_rom_erase && g_rom_program) ? 1 : 0;

    /* The USB controller is polled, not interrupt driven, and its state
     * machine must not run in interrupt context - hook it into the
     * scheduler's idle loop instead (see usb_idle_poll above). */
    g_idle_poll = usb_idle_poll;
}

/* ---- power ------------------------------------------------------------- */

void plat_shutdown(void)
{
    console_shutdown();

    /* No power-off on this board; stay in a polling loop so Ctrl-D on UART0
     * still reaches BOOTSEL after the kernel has stopped. */
    for (;;) {
        plat_board_panic_poll();
        for (volatile int i = 0; i < 200; i++)
            ;
    }
}

void plat_reboot(void)
{
    console_shutdown();
    rom_reboot_normal();
    plat_panic_halt();
}

uint32_t plat_board_cpu_id(void)
{
    return mmio_read32(SIO_CPUID);
}

/* /proc/cpuinfo: the core the image was built for and the measured
 * clk_sys (150 MHz after the PLL step). */
const char *plat_cpu_model(void)
{
    return "Hazard3 (RP2350)";
}

uint32_t plat_cpu_hz(void)
{
    return g_clk_hz;
}
