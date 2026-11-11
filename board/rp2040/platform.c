#include "platform.h"
#include "board.h"
#include "arm_memmap.h"
#include "usb_cdc.h"
#include "uart_dbg.h"
#include "console.h"
#include "string.h"
#include "cpu.h"

/* clk_sys is set once, in clocks_init(), to PLL_SYS at 125 MHz.  SysTick,
 * the PL011 divisor and /proc/cpuinfo all read this variable rather than a
 * compile-time constant, so a future board that has to fall back to the
 * 12 MHz crystal still gets a correct tick rate and baud. */
static uint32_t g_cpu_hz = 12000000u;
static uint32_t g_tick_hz = 1000u;
static volatile uint64_t g_ticks;
static uint32_t g_reload;

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
 * table).  A flash boot leaves XOSC off and clk_sys on ROSC (~5 MHz here),
 * NOT the 12 MHz crystal: clocks_init() in plat_board_early() enables XOSC,
 * repoints clk_ref at it and then moves clk_sys to PLL_SYS at 125 MHz, which
 * is what makes the UART baud, the SysTick rate, PLL_USB's 48 MHz reference
 * and the USB control-transfer turnaround all correct.  Exception vectors are
 * NOT repointed by the stage-2, so we set VTOR ourselves.
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
typedef void (*rp_rom_simple_t)(void);
typedef void *(*rp_rom_lookup_t)(uint16_t *table, uint32_t code);

static rp_rom_erase_t   g_rom_erase;
static rp_rom_program_t g_rom_program;
static rp_rom_flush_t   g_rom_flush;
static rp_rom_simple_t  g_rom_connect;
static rp_rom_simple_t  g_rom_exit_xip;
static int              g_rom_ready;   /* 0 until the whole set resolves  */

/* The 256-byte stage-2 must be re-entered from RAM after flash programming
 * disables XIP; 64 words is exactly what the ROM verifies. */
static uint32_t g_boot2_copy[64];

/* SMP flash handshake: core 0 disables XIP for the ROM erase/program call,
 * and every instruction fetch by core 1 would then be garbage.  Core 1 parks
 * in a RAM loop from its SysTick handler for the duration. */
static volatile int g_flash_park;
static volatile int g_flash_parked;

#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
static void rp_flash_park_loop(void);   /* defined with the flash RAM code */
#endif

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
    /* The ROM table already stores Thumb-callable addresses (bit 0 set).
     * Never OR the Thumb bit in BEFORE the NULL check: NULL|1 == 1 passes the
     * check and branches to address 1, which is a silent permanent hang.
     * That exact expression made both flash_range_program() and
     * reset_usb_boot() look like "ROM functions that never return". */
    return lookup(functab, code);
}

/* ---- clocks ------------------------------------------------------------- */

/* A flash boot leaves XOSC DISABLED and clk_sys on ROSC (~5 MHz measured
 * here).  Enable the crystal, repoint clk_ref at it, then start PLL_SYS and
 * move clk_sys onto 125 MHz.  That last step is not optional on this board:
 * at 12 MHz the host reset the USB port immediately after the first
 * GET_DESCRIPTOR(device) transfer, while rp2350 (150 MHz) enumerates at
 * once and pico-sdk/MicroPython both default the RP2040 to 125 MHz.
 *
 * The sequence mirrors pico-sdk's set_sys_clock_khz()/clock_configure():
 * park clk_sys on the glitchless clk_ref source first, program PLL_SYS,
 * change AUXSRC while SRC is still 0, and only then flip SRC to aux and wait
 * for CLK_SYS_SELECTED.  Writing SRC+AUXSRC in one store (the old runtime
 * Ctrl-K path) is a glitchy mux change and hung the core. */
static int pll_sys_start_125(void)
{
    uint32_t spins;

    /* Reset first so a PLL left configured by a previous run cannot leak.
     * RP2040 RESETS bit 12 is PLL_SYS (bit 14 is PWM). */
    spins = mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET);
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET, spins | RESET_PLL_SYS);
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET, spins & ~RESET_PLL_SYS);
    for (spins = 0; spins < 1000000u; spins++)
        if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & RESET_PLL_SYS)
            break;

    mmio_write32(PLL_SYS_CS, 1u);                    /* REFDIV = 1        */
    mmio_write32(PLL_SYS_FBDIV, 125u);               /* 12 * 125 MHz VCO  */
    mmio_write32(PLL_SYS_PWR, 0x2du);                /* all power down    */
    mmio_write32(PLL_SYS_PWR, 0x0cu);                /* PD + VCOPD off    */
    for (spins = 0; spins < 1000000u; spins++)
        if (mmio_read32(PLL_SYS_CS) & PLL_CS_LOCK)
            break;
    if (!(mmio_read32(PLL_SYS_CS) & PLL_CS_LOCK))
        return -1;

    mmio_write32(PLL_SYS_PRIM, (6u << 16) | (2u << 12)); /* /6 then /2    */
    mmio_write32(PLL_SYS_PWR, 0x04u);                /* POSTDIVPD off     */
    return 0;
}

static void clocks_init(void)
{
    uint32_t spins;
    int locked = 0;

    if (!(mmio_read32(XOSC_STATUS) & XOSC_STATUS_STABLE)) {
        mmio_write32(XOSC_CTRL, XOSC_CTRL_ENABLE_12M);
        for (spins = 0; spins < 1000000u; spins++)
            if (mmio_read32(XOSC_STATUS) & XOSC_STATUS_STABLE)
                break;
    }
    if (!(mmio_read32(XOSC_STATUS) & XOSC_STATUS_STABLE))
        goto keep_ref;                   /* no crystal: stay on ROSC      */

    mmio_write32(CLOCKS_BASE + CLK_REF_CTRL_OFFSET, CLK_REF_CTRL_SRC_XOSC);
    for (spins = 0; spins < 1000000u; spins++)
        if (mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) & (1u << 2))
            break;

    /* Start the 1 MHz tick generator inside the watchdog block.  pico-sdk's
     * runtime_init_clocks() does this through start_all_ticks(); without it
     * reset_usb_boot()'s watchdog_reboot() loads the counter but the tick
     * never advances, so the ROM does watchdog_reboot() then spins in
     * "while (true) __wfi();" forever - which is exactly the silent wedge
     * Ctrl-D produced.  WATCHDOG_TICK: CYCLES bits 8:0, ENABLE bit 9;
     * clk_ref is the 12 MHz crystal, so 12 gives 1 MHz. */
    /* Leave the watchdog tick stopped.  Starting it makes the bootrom's
     * leftover ~1 s watchdog count and reboots the board every ~0.5 s.
     * reset_usb_boot()'s own timer also never fires reliably here, so Ctrl-D
     * uses the USB-SOF force-trigger path in bootsel_rom_call() instead. */
    mmio_write32(0x40058000u,
                 mmio_read32(0x40058000u) & ~0x40000000u);

    /* Park clk_sys on clk_ref (12 MHz) and wait for the glitchless mux. */
    mmio_write32(CLOCKS_BASE + CLK_SYS_DIV_OFFSET, CLK_DIV_1);
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET, CLK_SYS_CTRL_SRC_CLKREF);
    for (spins = 0; spins < 1000000u; spins++)
        if (mmio_read32(CLOCKS_BASE + CLK_SYS_SELECTED_OFFSET) & 1u)
            break;

    if (pll_sys_start_125() == 0) {
        /* AUXSRC while SRC is 0, then flip the glitchless mux. */
        mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET,
                     (CLK_SYS_CTRL_AUXSRC_PLL_SYS << 5) |
                     CLK_SYS_CTRL_SRC_CLKREF);
        mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET,
                     (CLK_SYS_CTRL_AUXSRC_PLL_SYS << 5) |
                     CLK_SYS_CTRL_SRC_AUX);
        for (spins = 0; spins < 1000000u; spins++)
            if (mmio_read32(CLOCKS_BASE + CLK_SYS_SELECTED_OFFSET) & 2u) {
                locked = 1;
                break;
            }
    }
    if (locked)
        g_cpu_hz = RP2040_SYS_CLK_HZ;

keep_ref:
    /* RP2040 has no CLK_PERI_DIV register: clk_peri is clk_sys selected by
     * CLK_PERI_CTRL.AUXSRC and has no divider.  0x4c is reserved - writing
     * a "1.0" there is at best a no-op and at worst an extra divider. */
    mmio_write32(CLOCKS_BASE + CLK_PERI_CTRL_OFFSET,
                 CLK_PERI_CTRL_ENABLE |
                 (CLK_PERI_CTRL_AUXSRC_CLK_SYS << 5));
}

/* ---- bring-up probes ------------------------------------------------------ */
/* Raw UART writes: no console layer, no spin lock, no .bss-dependent state
 * beyond the one-shot flag.  A wedge anywhere between kmain() and the shell
 * still leaves a trace, which is how the RP2040 boot path is being mapped. */
#define BOOT_PROBE(tag) do {                                                \
        static int probe_seen;                                              \
        if (!probe_seen) { probe_seen = 1; dbg_uart_puts(tag "\n"); }       \
    } while (0)

/* ---- BOOTSEL escape hatch ------------------------------------------------ */

/* Ctrl-D asks the ROM to mount BOOTSEL again, so a wedged image can be
 * replaced without a human holding BOOTSEL + RESET.  RP2040's function is
 * ROM_TABLE_CODE('U','B') = 'U' | ('B' << 8) = 0x4255. */
typedef void (*rp_rom_usb_boot_t)(uint32_t gpio_mask, uint32_t disable_mask);
#define ROM_FUNC_RESET_USB_BOOT 0x4255u
#define BOOTSEL_MAGIC_CHAR      0x04u
#define REPLUG_CHAR             0x12u   /* Ctrl-R: fresh SE0 -> J cycle   */
#define TRACE_CHAR              0x14u   /* Ctrl-T: dump the USB IRQ trace */
#define E15_CHAR                0x05u   /* Ctrl-E: E15 workaround ON      */
#define E15_OFF_CHAR            0x17u   /* Ctrl-W: E15 workaround OFF     */
#define BOOTSEL_ROM_CHAR        0x02u   /* Ctrl-B: test ROM call directly */

#define WD_BASE         0x40058000u
#define WD_CTRL_REG     (*(volatile uint32_t *)(WD_BASE + 0x00u))
#define WD_SCRATCH(n)   (*(volatile uint32_t *)(WD_BASE + 0x0cu + 4u * (n)))
#define WD_CTRL_TRIGGER 0x80000000u
#define PSM_WDSEL_REG   (*(volatile uint32_t *)(0x40010000u + 0x08u))
#define BOOTSEL_SCRATCH_MAGIC 0x544f5342u   /* "TOSB"                    */

static volatile int g_replug_req, g_trace_req;
static volatile int g_bootsel_req;
static volatile int g_bootsel_rom_req;
static volatile int g_bootsel_force;

/* Called from the USB trampoline while reset_usb_boot() is spinning in wfi.
 * The ROM wrote scratch[4..7] and enabled the watchdog timer; on this chip
 * the timer never fires (the ROM assumes watchdog_reboot's countdown works),
 * so a still-live USB interrupt forces WATCHDOG_CTRL.TRIGGER instead.  The
 * scratch magic makes the bootrom jump to _usb_boot_reboot_wrapper. */
int plat_bootsel_force_check(void)
{
    if (!g_bootsel_force)
        return 0;
    if (mmio_read32(0x4005801cu) != 0xb007c0d3u)  /* scratch[4] */
        return 0;                                /* ROM not there yet */
    g_bootsel_force = 0;
    return 1;
}

/* Measured failure: every console path that sees Ctrl-D runs in SVCall
 * (IPSR=0x0b), and a ROM call from ANY exception context wedges the board --
 * the ROM's USB interrupt cannot preempt the exception that invoked it on
 * Cortex-M0+.  Trying to defer to thread mode does not help because the
 * console syscall stays in SVCall for its whole lifetime.
 *
 * The guaranteed route is a watchdog reset: it puts every peripheral back
 * into its power-on state (including a clean USB disconnect), and the very
 * first thing plat_board_early() does is check a scratch register and hand
 * over to the ROM in THREAD mode, before clocks/VTOR/USB are touched at all.
 * That is bit-for-bit the state of a physical BOOT+RESET press. */
static void bootsel_request(void)
{
    dbg_uart_puts("[bootsel] watchdog reboot\n");
    /* Let the host see a real disconnect before the reset.  USBCTRL is reset
     * by PSM_WDSEL, but the ROM re-attaches within milliseconds; a host that
     * missed SE0 keeps the old device node and never enumerates the ROM's
     * BOOTSEL volume.  200 ms is the same window usb_cdc_init() uses. */
    usb_cdc_disconnect();
    for (volatile uint32_t i = 0; i < (g_cpu_hz / 3000u) * 200u; i++)
        ;
    WD_SCRATCH(0) = BOOTSEL_SCRATCH_MAGIC;
    /* scratch[4..7] are the ROM's "reboot to pc/sp" magic; clear them so a
     * plain flash boot follows if the handover itself ever fails. */
    for (unsigned i = 1; i < 8u; i++)
        WD_SCRATCH(i) = 0u;
    /* PSM_WDSEL resets to 0 (= reset nothing).  Mirror pico-sdk's
     * _watchdog_enable(): reset all domains except ROSC/XOSC so the crystal
     * keeps running (PSM_WDSEL_BITS & ~(ROSC|XOSC) == 0x0001fffc). */
    PSM_WDSEL_REG = 0x0001fffcu;
    /* UART0 is one of the domains the watchdog resets.  Let the last marker
     * actually leave the FIFO first, otherwise it is truncated to a couple
     * of framing-error bytes and the log looks like a silent wedge. */
    for (uint32_t i = 0; i < 300000u; i++) {
        uint32_t fr = mmio_read32(RP_UART_FR);
        /* TXFE is bit 7 (bit 3 is BUSY).  Wait for BOTH "FIFO empty" and
         * "shifter idle", otherwise the watchdog reset truncates the last
         * marker into a framing-error byte. */
        if ((fr & RP_UART_FR_TXFE) && !(fr & RP_UART_FR_BUSY))
            break;
    }
    WD_CTRL_REG = WD_CTRL_TRIGGER;      /* immediate, self-clearing */
    for (;;)
        ;
}



/* Direct ROM hand-over from thread mode.  Every previous attempt called
 * reset_usb_boot() from SVCall (IPSR=11) or routed through a watchdog
 * reboot; the watchdog route gets as far as the ROM but the host never sees
 * a BOOTSEL volume.  pico-sdk's reset_usb_boot() is designed to be called
 * from a normal thread by an application, so try exactly that: the idle
 * loop's tty_poll() reaches plat_console_getchar() with IPSR=0.  If an SVC
 * path sees the request first, it returns and the idle loop retries. */
static void bootsel_try_direct(void)
{
    rp_rom_usb_boot_t boot;
    uint32_t ipsr;

    if (!g_bootsel_req)
        return;
    __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
    if (ipsr != 0u)
        return;                         /* not thread: let the idle loop run */
    g_bootsel_req = 0;
    dbg_uart_puts("[bootsel] direct ROM from thread\n");
    boot = (rp_rom_usb_boot_t)rp_rom_func(ROM_FUNC_RESET_USB_BOOT);
    if (!boot) {
        dbg_uart_puts("[bootsel] ROM lookup failed\n");
        return;
    }
    /* Drop D+ first so the host tears down the (now mounted) CDC node.
     * ~100 ms at the current clk_sys; the tick is about to be stopped. */
    usb_cdc_disconnect();
    for (volatile uint32_t i = 0; i < (g_cpu_hz / 3000u) * 100u; i++)
        ;
    for (uint32_t i = 0; i < 300000u; i++) {
        uint32_t fr = mmio_read32(RP_UART_FR);
        if ((fr & RP_UART_FR_TXFE) && !(fr & RP_UART_FR_BUSY))
            break;
    }
    mmio_write32(ARM_SYSTICK_CTRL, 0u);
    mmio_write32(ARM_VTOR, 0u);
    mmio_write32(ARM_NVIC_ICER(0), 0xFFFFFFFFu);
    mmio_write32(ARM_SCS_BASE + 0x280u, 0xFFFFFFFFu);  /* ICPR0 */
    __asm__ volatile("cpsie i" ::: "memory");
    boot(0u, 0u);
    dbg_uart_puts("[bootsel] ROM returned\n");
}

/* Called from the top of plat_board_early(); never returns on a hit.
 * The previous version handed the ROM a live VTOR (ours, in XIP flash) and
 * leftover NVIC/SysTick state: the ROM's USB IRQ then vectored into our dead
 * handler and the board wedged with no BOOTSEL volume.  Give it back the ROM
 * vector table, clear every enable/pending bit, stop SysTick and leave
 * PRIMASK clear - the ROM bootloader needs its own interrupts to run. */
static void bootsel_early_handover(void)
{
    rp_rom_usb_boot_t boot;
    uint32_t ipsr;

    if (WD_SCRATCH(0) != BOOTSEL_SCRATCH_MAGIC)
        return;
    WD_SCRATCH(0) = 0u;                /* consume before the ROM starts   */
    __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
    dbg_uart_puts("[bootsel] early handover ipsr=");
    dbg_uart_hex(ipsr);
    dbg_uart_puts("\n");
    boot = (rp_rom_usb_boot_t)rp_rom_func(ROM_FUNC_RESET_USB_BOOT);
    dbg_uart_puts("[bootsel] ptr=");
    dbg_uart_hex((uint32_t)(uintptr_t)boot);
    dbg_uart_puts("\n");
    if (!boot)
        return;                         /* fall through to a normal boot   */
    dbg_uart_puts("[bootsel] calling ROM\n");
    for (uint32_t i = 0; i < 300000u; i++) {
        uint32_t fr = mmio_read32(RP_UART_FR);
        /* TXFE is bit 7 (bit 3 is BUSY).  Wait for BOTH "FIFO empty" and
         * "shifter idle", otherwise the watchdog reset truncates the last
         * marker into a framing-error byte. */
        if ((fr & RP_UART_FR_TXFE) && !(fr & RP_UART_FR_BUSY))
            break;
    }
    /* Keep the running clock tree intact for the ROM.  QMK's RP2040
     * bootloader calls reset_usb_boot() only after clocks_init(), with the
     * warning "all clocks have to be enabled ... otherwise the bootrom will
     * be stuck infinitely".  Earlier rounds switched clk_sys back to 12 MHz
     * and powered PLL_SYS down here, which is the one state the ROM has
     * never been fed.  Leave clk_sys at 125 MHz, PLL_SYS on and clk_peri
     * following it; the ROM's own reboot sets up whatever it needs. */
    mmio_write32(ARM_SYSTICK_CTRL, 0u);
    mmio_write32(ARM_VTOR, 0u);
    mmio_write32(ARM_NVIC_ICER(0), 0xFFFFFFFFu);
    mmio_write32(ARM_SCS_BASE + 0x280u, 0xFFFFFFFFu);  /* ICPR0           */
    __asm__ volatile("cpsie i" ::: "memory");
    boot(0u, 0u);                      /* no activity LED, both ifaces on */
    /* reset_usb_boot is documented noreturn; if it ever returns, say so
     * instead of spinning with the UART still clocked for 125 MHz. */
    dbg_uart_init(12000000u);
    dbg_uart_puts("[bootsel] early ROM returned\n");
    for (;;)
        ;
}

/* UART0 RX is polled from the tick, from console output and from the panic
 * loop, so Ctrl-D reaches the ROM even when the shell is stuck.  Reading DR
 * consumes the byte, so harvested characters are queued for the shell rather
 * than discarded - a bare 1 kHz poll would otherwise eat every keystroke.
 * Single producer / single consumer, 64 bytes is plenty for typed input. */
#define RXQ_SIZE 64u
static volatile char rxq[RXQ_SIZE];
static volatile unsigned rxq_head, rxq_tail;

static int uart0_hw_getchar(void)
{
    if (mmio_read32(RP_UART_FR) & RP_UART_FR_RXFE)
        return -1;
    return (int)(mmio_read32(RP_UART_DR) & 0xffu);
}

static void uart0_rx_push(char c)
{
    uint32_t primask;
    unsigned n;

    /* bootsel_poll_uart() is called from SysTick, SVC and the panic loop, so
     * this is a multi-producer ring.  The head update is a read-modify-write
     * and must not interleave with an interrupt doing the same. */
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");
    n = (rxq_head + 1u) % RXQ_SIZE;
    if (n != rxq_tail) {
        rxq[rxq_head] = c;
        rxq_head = n;
    }
    __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
}

static void bootsel_poll_uart(void);   /* defined just below */

static int uart0_getchar(void)
{
    /* Always route the hardware byte through the poller: it is the single
     * place that consumes the bring-up control characters, so a byte that
     * arrives between two ticks can never slip straight into the shell. */
    bootsel_poll_uart();
    if (rxq_tail != rxq_head) {
        int c = rxq[rxq_tail];

        rxq_tail = (rxq_tail + 1u) % RXQ_SIZE;
        return c;
    }
    return -1;
}

/* Direct ROM call from the running system (SysTick/thread context both land
 * here).  QMK's RP2040 bootloader jump calls reset_usb_boot() only after
 * clocks_init(), with the comment "all clocks have to be enabled before
 * jumping to the bootloader function, otherwise the bootrom will be stuck
 * infinitely".  The earlier failures all called it in the early boot path
 * after switching clk_sys back to 12 MHz and powering PLL_SYS down.  Call it
 * with the system exactly as it runs: 125 MHz, PLL_USB alive, clk_usb alive. */
static void bootsel_rom_call(void)
{
    rp_rom_usb_boot_t boot;

    g_bootsel_req = 0;
    dbg_uart_puts("[bootsel] ROM call (clocks running)\n");
    boot = (rp_rom_usb_boot_t)rp_rom_func(ROM_FUNC_RESET_USB_BOOT);
    if (!boot) {
        dbg_uart_puts("[bootsel] ROM lookup failed\n");
        return;
    }
    /* Keep the CDC attached: SOF must keep arriving so the USB IRQ can force
     * the watchdog after reset_usb_boot() has written scratch[4..7].  The
     * previous 200 ms D+ drop here removed the only interrupt source and the
     * ROM sat in wfi forever.  The reset itself gives the host its SE0. */
    for (uint32_t i = 0; i < 300000u; i++) {
        uint32_t fr = mmio_read32(RP_UART_FR);
        if ((fr & RP_UART_FR_TXFE) && !(fr & RP_UART_FR_BUSY))
            break;
    }
    /* TinyUSB does not enable SOF interrupts normally, so a mounted but idle
     * CDC device produces no IRQ once reset_usb_boot() is spinning in wfi.
     * Enable DEV_SOF here: the 1 ms frame boundary is the heartbeat that
     * lets the trampoline force the watchdog. */
    mmio_write32(0x50110000u + 0x90u,
                 mmio_read32(0x50110000u + 0x90u) | 0x00020000u);
    g_bootsel_force = 1;      /* USB IRQ forces the reset after scratch[4] */
    boot(0u, 0u);
    dbg_uart_puts("[bootsel] ROM returned\n");
}

/* Also installed as g_panic_poll so it keeps working after the kernel dies. */
static void bootsel_poll_uart(void)
{
    int c = uart0_hw_getchar();

    if (c < 0)
        return;
    switch (c) {
    case BOOTSEL_MAGIC_CHAR:
        g_bootsel_req = 1;       /* serviced from the idle thread */
        return;
    case BOOTSEL_ROM_CHAR:
        g_bootsel_rom_req = 1;
        return;
    case REPLUG_CHAR: g_replug_req = 1; return;
    case TRACE_CHAR:  g_trace_req  = 1; return;
    case E15_CHAR:
        usb_cdc_set_e15(1);
        dbg_uart_puts("[usb] E15 forced-J workaround ON\n");
        return;
    case E15_OFF_CHAR:
        usb_cdc_set_e15(0);
        dbg_uart_puts("[usb] E15 forced-J workaround OFF\n");
        return;
    default: break;
    }
    uart0_rx_push((char)c);
}

/* ---- console: USB CDC-ACM ------------------------------------------------ */

/* UART0 (GPIO0/1, wired to a USB-UART on this board) is the primary console.
 * The original port sent every console byte to the CDC FIFO instead.  When
 * the host does not enumerate, usb_cdc_tx_room() reports 0 forever and the
 * console layer spin-waits inside banner(), so the boot dies just after
 * "[usb] console init returned": no shell prompt, no ticks, no way to wake
 * it.  The CDC port is still initialised and mirrored onto the same stream,
 * so a host that attaches later sees the shell too. */

/* Defined in the boot section below; the console turns it off once the
 * host has enumerated. */
static void led_set(int on);

/* One line of raw USB state, written straight to the debug UART so it never
 * recurses through the console layer.  A zero irq count with ints != 0 means
 * the vector is not being taken; a nonzero count with mnt=0 means the host
 * is resetting the port after each descriptor. */
static void usb_dump(const char *tag)
{
    dbg_uart_puts(tag);
    dbg_uart_puts(" st=");     dbg_uart_hex(usb_cdc_init_stage());
    dbg_uart_puts(" mnt=");    dbg_uart_hex(usb_cdc_dbg(3));
    dbg_uart_puts(" sie=");    dbg_uart_hex(usb_cdc_dbg(0));
    dbg_uart_puts("/");        dbg_uart_hex(usb_cdc_dbg(1));
    dbg_uart_puts(" main=");   dbg_uart_hex(usb_cdc_dbg(2));
    dbg_uart_puts(" ints=");   dbg_uart_hex(usb_cdc_dbg(6));
    dbg_uart_puts(" raw=");    dbg_uart_hex(usb_cdc_dbg(24));
    dbg_uart_puts(" irq=");    dbg_uart_hex(usb_cdc_dbg(16));
    dbg_uart_puts(" iser=");   dbg_uart_hex(usb_cdc_dbg(8));
    dbg_uart_puts(" inte=");   dbg_uart_hex(usb_cdc_dbg(19));
    dbg_uart_puts(" buf=");    dbg_uart_hex(usb_cdc_dbg(20));
    dbg_uart_puts(" task=");   dbg_uart_hex(usb_cdc_dbg(21));
    dbg_uart_puts("/");        dbg_uart_hex(usb_cdc_dbg(22));
    dbg_uart_puts(" busy=");   dbg_uart_hex(usb_cdc_dbg(23));
    dbg_uart_puts(" e15=");    dbg_uart_hex(usb_cdc_dbg(25));
    dbg_uart_puts("/");        dbg_uart_hex(usb_cdc_dbg(26));
    dbg_uart_puts(" clk=");    dbg_uart_hex(mmio_read32(0x40008054u));
    dbg_uart_puts("/");        dbg_uart_hex(mmio_read32(0x40008058u));
    dbg_uart_puts(" pll=");    dbg_uart_hex(mmio_read32(0x4002c000u));
    dbg_uart_puts("/");        dbg_uart_hex(mmio_read32(0x4002c004u));
    dbg_uart_puts("/");        dbg_uart_hex(mmio_read32(0x4002c008u));
    dbg_uart_puts("/");        dbg_uart_hex(mmio_read32(0x4002c00cu));
    dbg_uart_puts(" syst=");   dbg_uart_hex(mmio_read32(ARM_SYSTICK_CTRL));
    dbg_uart_puts("/");        dbg_uart_hex(mmio_read32(ARM_SYSTICK_LOAD));
    dbg_uart_puts("/");        dbg_uart_hex(mmio_read32(ARM_SYSTICK_VAL));
    dbg_uart_puts(" cpu=");    dbg_uart_hex(g_cpu_hz);
    dbg_uart_puts("/");        dbg_uart_hex(g_reload);
    dbg_uart_puts(" tk=");     dbg_uart_hex((uint32_t)g_ticks);
    dbg_uart_puts("/");        dbg_uart_hex((uint32_t)(g_ticks >> 32));
    dbg_uart_puts("\n");
}

void plat_console_init(void)
{
    dbg_uart_puts("[console] uart0 primary\n");
    /* This argument scales the bounded enumeration wait; it is the CPU
     * clock, not the USB PHY's 48 MHz. */
    usb_cdc_console_init(g_cpu_hz);
    /* The NVIC gate is opened inside usb_cdc_init(), before the bounded
     * enumeration wait - see the comment there. */
    dbg_uart_puts("[usb] init done\n");
    usb_dump("[usb] ");
    if (usb_cdc_configured())
        led_set(0);
}

void plat_console_putc(char c)
{
    BOOT_PROBE("[p] console putc");
    /* Harvest input on every console write too: during early boot the tick
     * is still masked, so this is the only place Ctrl-D can be seen. */
    bootsel_poll_uart();
    /* Bounded wait: a gated clk_peri can leave FR stuck at "FIFO full", and
     * dropping every byte would turn the boot log into a black box.  Write
     * even if the bound expires. */
    for (uint32_t spins = 0; spins < 100000u; spins++)
        if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF))
            break;
    mmio_write32(RP_UART_DR, (uint8_t)c);
    usb_cdc_putc(c);                 /* no-op until the host enumerates */
}

void plat_console_flush(void)
{
    /* Wait for TXFE (bit 7), not BUSY (bit 3): the FIFO can be empty while
     * the shift register is still sending the last character. */
    for (uint32_t spins = 0; spins < 100000u; spins++)
        if (!(mmio_read32(RP_UART_FR) & RP_UART_FR_TXFE))
            break;
    usb_cdc_flush();
}

int plat_console_tx_room(void)
{
    /* Only the UART paces the kernel.  Asking the CDC FIFO for room would
     * deadlock the boot whenever no host is attached. */
    return (mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF) ? 0 : 16;
}

int plat_console_getchar(void)
{
    int c;

    bootsel_try_direct();
    c = uart0_getchar();
    if (c >= 0)
        return c;

    usb_cdc_poll();
    return usb_cdc_getchar();
}

/* ---- SysTick ----------------------------------------------------------- */

/* clocks_init() leaves clk_sys (and therefore the SysTick input) on PLL_SYS
 * at 125 MHz, so a reload of CLK/hz gives `hz` ticks per second.  The
 * counter is started here with the interrupt masked; plat_irq_enable() later
 * unmasks it (same ordering the rv32 port gets from mstatus.MIE). */

void plat_timer_init(uint32_t hz)
{
    BOOT_PROBE("[p] timer init");
    g_tick_hz = hz;
    g_reload = g_cpu_hz / hz;
    g_ticks = 0;

    mmio_write32(ARM_SYSTICK_LOAD, g_reload - 1u);
    mmio_write32(ARM_SYSTICK_VAL, 0);
    /* ENABLE | TICKINT | CLKSOURCE (processor clock).  TICKINT is set here
     * too: PRIMASK still masks it until plat_irq_enable(), and writing all
     * three bits makes sure CLKSOURCE cannot be left at the 12 MHz reference
     * (which made the tick ~70 Hz instead of 1 kHz). */
    mmio_write32(ARM_SYSTICK_CTRL, 7u);
}

uint64_t plat_ticks(void)
{
    return g_ticks;
}

uint32_t plat_tick_hz(void)
{
    return g_cpu_hz / g_reload;
}

uint32_t plat_uptime_ms(void)
{
    return (uint32_t)((g_ticks * 1000u) / (uint64_t)plat_tick_hz());
}

/* Clock: the tick gives whole periods, SysTick's live down-counter gives
 * the position inside the current one, so resolution is 1/125 MHz (8 ns)
 * rather than 1 ms.  Clamped against the wrap that happens between the
 * reload and the tick interrupt. */
uint64_t plat_now_ns(void)
{
    static uint64_t last_ns;
    uint32_t load = g_reload;
    uint32_t val  = mmio_read32(ARM_SYSTICK_VAL) & 0xFFFFFFu;
    uint32_t into = (load > val) ? (load - val) : 0u;
    uint64_t cycles = g_ticks * (uint64_t)load + into;
    /* Work in thousandths: ns = cycles * 1000000 / CLK, split to avoid
     * overflow on 32-bit arithmetic. */
    uint64_t per_ms = g_cpu_hz / 1000u;
    uint64_t ns = (cycles / per_ms) * 1000000ull
                + ((cycles % per_ms) * 1000000ull) / per_ms;

    if (ns < last_ns)
        ns = last_ns;
    last_ns = ns;
    return ns;
}

void plat_timer_tick(void)
{
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    /* Core 0 is about to disable XIP for a ROM flash call.  Park here, in a
     * RAM loop, before any instruction fetch can touch flash. */
    if (g_flash_park)
        rp_flash_park_loop();
#endif
    /* Every core has its own SysTick - that is what preempts whatever that
     * core is running - but only core 0 owns the wall clock, USB, the LED
     * and the BOOTSEL/console poll.  Letting both increment g_ticks would
     * run time and every sleep deadline at 2x. */
    if (plat_cpu_id() != 0)
        return;

    BOOT_PROBE("[p] first tick");
    /* Something in the bootrom/flash path keeps re-arming a ~0.5 s watchdog;
     * with the tick generator alive it resets the board during normal use.
     * Keep it disarmed every tick.  The Ctrl-D path uses the USB-SOF force
     * trigger, not this timer. */
    if (mmio_read32(0x40058000u) & 0x40000000u)
        mmio_write32(0x40058000u, 0u);
    g_ticks++;
    /* bootsel_poll_uart() queues every byte it harvests, so the tick cannot
     * steal keystrokes from plat_console_getchar(); it only makes Ctrl-D
     * work while the shell is blocked on a device. */
    bootsel_poll_uart();
    if (g_bootsel_req) {
        g_bootsel_req = 0;
        dbg_uart_puts("[bootsel] Ctrl-D disabled: use the BOOTSEL button\n");
    }
    if (g_bootsel_rom_req) {
        g_bootsel_rom_req = 0;
        dbg_uart_puts("[bootsel] Ctrl-B disabled: use the BOOTSEL button\n");
    }
    usb_cdc_poll();
    if (g_replug_req) {
        g_replug_req = 0;
        dbg_uart_puts("[usb] replug cycle\n");
        usb_cdc_replug_cycle();
    }
    if (g_trace_req) {
        g_trace_req = 0;
        usb_dump("[usbt] ");
        usb_cdc_trace_dump();
    }
    /* Two late snapshots: by 1 s the host has had time to enumerate with
     * interrupts unmasked (they are still masked during the early wait). */
    if (g_ticks == 1000u || g_ticks == 3000u) {
        usb_dump("[usb@] ");
        usb_cdc_trace_dump();
    }
    /* SysTick reloads itself; nothing to re-arm. */
}

/* Called on a secondary core from cpu_secondary_main(), before its first
 * plat_irq_enable().  SysTick's LOAD/VAL/CTRL are per-core registers. */
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
void plat_timer_start_cpu(void)
{
    if (g_reload == 0)
        return;
    mmio_write32(ARM_SYSTICK_LOAD, g_reload - 1u);
    mmio_write32(ARM_SYSTICK_VAL, 0);
    mmio_write32(ARM_SYSTICK_CTRL, 7u);     /* ENABLE | TICKINT | CLKSOURCE */
}

/* ---- second core -------------------------------------------------------- */

/* Core 1 does not execute our reset vector: the RP2040 boot ROM leaves it in
 * the SIO FIFO wait loop (wait_for_vector in bootrom_rt0.S).  The protocol is
 * the same one pico-sdk's multicore_launch_core1_raw() uses: drain, then
 * 0, 0, 1, <vector table>, <stack>, <entry>, with every word echoed.  The two
 * leading zeros flush stale words from a previous launch attempt; a mismatch
 * restarts the sequence. */
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
    __asm__ volatile("sev" ::: "memory");
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
        return 0;

    tail[0] = (uint32_t)(uintptr_t)__vector_table;
    tail[1] = CPU_KSTACK_TOP(1u);
    tail[2] = (uint32_t)(uintptr_t)arm_core1_entry | 1u;  /* Thumb bit */

    for (;;) {
        cmd = (seq < 3) ? lead[seq] : tail[seq - 3];

        if (cmd == 0) {
            sio_fifo_drain();
            __asm__ volatile("sev" ::: "memory");
        }
        sio_fifo_push(cmd);

        if (sio_fifo_pop() != cmd) {
            seq = 0;
            if (++restarts > 4)
                return 0;
            continue;
        }
        if (++seq == 6)
            return 1;           /* core 1 is running arm_core1_entry() */
    }
}
#endif /* TOS_MAX_CPUS > 1 */

/* ---- block device: NOR flash via boot ROM ------------------------------ */

/* The build tool concatenates the littlefs image into the flash at offset
 * 0x80000 (see emitRP2040UF2 in internal/build/uf2.go), which is exactly
 * PLAT_LFS_LBA * 512.  lba passed by ramdisk.c is already global
 * (PLAT_LFS_LBA + sector), so the flash byte address is simply lba * 512.
 * Erase operates on 4 KiB sectors (8 littlefs sectors); the ROM function
 * requires the address aligned to 4 KiB and the count a multiple of 4 KiB. */

#define RP_FLASH_SECTOR   4096u
#define RP_SECTOR_ERASE_CMD 0x20u          /* W25Qxx 4 KiB sector erase   */

/* The ROM's flash_range_erase/program run with XIP disabled, so the caller
 * cannot execute from flash between flash_exit_xip() and the boot2 copy
 * re-entering XIP.  This function is therefore placed in .data (copied to
 * SRAM by _start), copies the 256-byte stage-2 out of XIP first, and drives
 * the same sequence pico-sdk's flash_range_program() uses:
 *
 *   connect_internal_flash -> flash_exit_xip -> erase/program
 *   -> flash_flush_cache -> call the RAM copy of boot2
 *
 * The function pointers live in .data as well, so no dereference touches
 * XIP while the SSI is in serial command mode. */
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
/* Runs on core 1 from its SysTick handler.  .data.flash_ram is copied to
 * SRAM, so this loop survives core 0 turning XIP off. */
static void __attribute__((noinline, section(".data.flash_ram")))
rp_flash_park_loop(void)
{
    g_flash_parked = 1;
    __asm__ volatile("dsb" ::: "memory");
    /* Busy-spin, not wfe: clearing g_flash_park and issuing sev is not
     * atomic with entering wfe, and a lost wakeup leaves this core asleep
     * inside the SysTick handler (interrupts masked) forever.  The flash
     * call is one erase/program, so the burn is short. */
    for (volatile uint32_t spins = 0; spins < 200000000u; spins++) {
        if (!g_flash_park)
            break;
    }
    g_flash_parked = 0;
}

/* Called from plat_cpu_idle() on the other core.  That path is needed
 * because a core parked in sched_idle() is already inside its SysTick
 * exception and will not take another tick, so the tick-side check alone
 * can never see g_flash_park on an idle core. */
void plat_flash_park_poll(void)
{
    if (g_flash_park)
        rp_flash_park_loop();
}

static int rp_flash_park_other(void)
{
    if (cpu_count <= 1)
        return 0;
    g_flash_parked = 0;
    g_flash_park = 1;
    __asm__ volatile("sev" ::: "memory");
    for (uint32_t i = 0; i < 20000000u; i++)
        if (g_flash_parked)
            return 0;
    g_flash_park = 0;
    __asm__ volatile("sev" ::: "memory");
    dbg_uart_puts("[flash] park timeout\n");
    return -1;                  /* core 1 never parked: do not kill XIP */
}

static void rp_flash_unpark_other(void)
{
    g_flash_park = 0;
    __asm__ volatile("sev" ::: "memory");
}
#endif

static int __attribute__((noinline, section(".data.flash_ram")))
rp_flash_op(uint32_t addr, const uint8_t *data, uint32_t count, int erase)
{
    const volatile uint32_t *src = (const volatile uint32_t *)XIP_BASE;
    volatile uint32_t *dst = g_boot2_copy;
    uint32_t i;

#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    if (rp_flash_park_other() != 0)
        return -1;
#endif

    for (i = 0; i < 64u; i++)
        dst[i] = src[i];
    __asm__ volatile("dsb\n isb" ::: "memory");

    g_rom_connect();
    g_rom_exit_xip();
    if (erase)
        g_rom_erase(addr, count, RP_FLASH_SECTOR, RP_SECTOR_ERASE_CMD);
    else
        g_rom_program(addr, data, count);
    g_rom_flush();
    ((void (*)(void))((uintptr_t)g_boot2_copy + 1u))();
    __asm__ volatile("dsb\n isb" ::: "memory");

#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    rp_flash_unpark_other();
#endif
    return 0;
}

int plat_block_read(uint32_t lba, uint32_t count, void *buf)
{
    BOOT_PROBE("[p] bd read");
    /* Reads go straight through the XIP mapping; the cache serves them once
     * flushed after the most recent write. */
    const void *src = (const void *)(XIP_BASE + lba * 512u);
    memcpy(buf, src, count * 512u);
    return 0;
}

int plat_block_erase(uint32_t lba, uint32_t count)
{
    BOOT_PROBE("[p] bd erase");
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
    int rc = rp_flash_op(addr, NULL, end - addr, 1);
    if (!(was_enabled & 1u))
        __asm__ volatile("cpsie i" ::: "memory");
    return rc;
}

int plat_block_write(uint32_t lba, uint32_t count, const void *buf)
{
    BOOT_PROBE("[p] bd write");
    uint32_t addr, was_enabled;

    if (!g_rom_ready)
        return -1;

    /* lba is always 512-aligned and count*512 is a multiple of 256, satisfying
     * the ROM program alignment rules.  The bounce buffer in ramdisk.c is in
     * SRAM, so the ROM reads its source from RAM (never from XIP). */
    addr = lba * 512u;

    __asm__ volatile("mrs %0, primask" : "=r"(was_enabled));
    __asm__ volatile("cpsid i" ::: "memory");
    int rc = rp_flash_op(addr, (const uint8_t *)buf, count * 512u, 0);
    if (!(was_enabled & 1u))
        __asm__ volatile("cpsie i" ::: "memory");
    return rc;
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
    /* PADS_BANK0: offset 0 is VOLTAGE_SELECT, so GPIO n starts at +4+4*n.
     * The old code used +4*n and programmed GPIO24's pad instead. */
    uint32_t p = PADS_BANK0_BASE + 0x04u + 4u * RP_LED_GPIO;
    uint32_t pad = mmio_read32(p);

    /* Pad: input enable on (bit 6), output disable off (bit 7).  The bank
     * itself is out of reset - dbg_uart_init() released IO_BANK0 (bit 5),
     * PADS_BANK0 (bit 8) and UART0 (bit 22) before we got here. */
    mmio_write32(p, (pad | (1u << 6)) & ~(1u << 7));
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
    /* Clocks first: XOSC + clk_ref = 12 MHz + PLL_SYS/clk_sys = 125 MHz,
     * otherwise every UART divisor, SysTick reload, PLL_USB reference and
     * USB control-transfer turnaround is wrong.  A cold BOOTSEL boot also
     * has XOSC running (the ROM needs it for its own USB PLL), so doing
     * this before the handover does not move us away from the reset state. */
    clocks_init();
    g_panic_poll = bootsel_poll_uart;

    /* UART0 on GPIO0/1 is the bring-up console.  It also proves the RESETS
     * bits and the pad mux before USB is even attempted.  The GP0 square
     * wave that used to run here was removed: GP0 /is/ the UART TX pin, so
     * the beacon only produced a burst of framing errors on the host. */
    dbg_uart_init(g_cpu_hz);
    dbg_uart_puts(g_cpu_hz == RP2040_SYS_CLK_HZ
                      ? "\n[rp2040] boot clk=125MHz\n"
                      : "\n[rp2040] boot clk=12MHz (PLL_SYS failed)\n");

    /* A Ctrl-D left a magic in the watchdog scratch register and triggered a
     * watchdog reset.  Hand over to the ROM now, in thread mode with a live
     * console so the handover itself is observable.  Still before VTOR is
     * repointed and before any interrupt is enabled. */
    bootsel_early_handover();

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
    BOOT_PROBE("[p] board init");
    /* Resolve the boot ROM flash API.  If any entry is missing (for example a
     * simulator without a boot ROM) the block device degrades to read-only and
     * the littlefs write self-test reports FAILED instead of crashing. */
    g_rom_erase   = (rp_rom_erase_t)rp_rom_func(ROM_FUNC_FLASH_RANGE_ERASE);
    BOOT_PROBE("[p] rom lookup 1");
    g_rom_program = (rp_rom_program_t)rp_rom_func(ROM_FUNC_FLASH_RANGE_PROGRAM);
    BOOT_PROBE("[p] rom lookup 2");
    g_rom_flush   = (rp_rom_flush_t)rp_rom_func(ROM_FUNC_FLASH_FLUSH_CACHE);
    BOOT_PROBE("[p] rom lookup 3");
    g_rom_connect = (rp_rom_simple_t)rp_rom_func(ROM_FUNC_CONNECT_INTERNAL_FLASH);
    BOOT_PROBE("[p] rom lookup 4");
    g_rom_exit_xip = (rp_rom_simple_t)rp_rom_func(ROM_FUNC_FLASH_EXIT_XIP);
    BOOT_PROBE("[p] rom lookup 5");
    dbg_uart_puts("[rom] erase=");
    dbg_uart_hex((uint32_t)(uintptr_t)g_rom_erase);
    dbg_uart_puts(" program=");
    dbg_uart_hex((uint32_t)(uintptr_t)g_rom_program);
    dbg_uart_puts(" flush=");
    dbg_uart_hex((uint32_t)(uintptr_t)g_rom_flush);
    dbg_uart_puts(" connect=");
    dbg_uart_hex((uint32_t)(uintptr_t)g_rom_connect);
    dbg_uart_puts(" exit_xip=");
    dbg_uart_hex((uint32_t)(uintptr_t)g_rom_exit_xip);
    dbg_uart_puts("\n");
    g_rom_ready = (g_rom_erase && g_rom_program && g_rom_flush &&
                   g_rom_connect && g_rom_exit_xip) ? 1 : 0;

    /* ROM flash writes are NOT callable from XIP code: once flash_exit_xip()
     * has run, the next instruction fetch through 0x10000000 faults.  The
     * whole sequence lives in rp_flash_op() in .data (SRAM) and finishes by
     * calling a RAM copy of boot2 to re-enter XIP.  If any ROM entry is
     * missing, leave the block device read-only instead of taking that path. */
    BOOT_PROBE(g_rom_ready ? "[p] flash writes enabled (RAM funcs)"
                           : "[p] flash writes disabled (ROM lookup failed)");
}

/* Called from plat_irq_enable() the first time interrupts come on.  TICKINT
 * stays clear until then so a tick cannot arrive before the scheduler exists. */
void plat_timer_irq_unmask(void)
{
    /* Core 1 reaches this too (via plat_irq_enable); only core 0 should
     * print the probe, otherwise it interleaves with cpu_init's [smp] line. */
    if (plat_cpu_id() == 0)
        BOOT_PROBE("[p] tick unmask");
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

/* The stage-2 leaves clk_sys at ROSC; clocks_init() moves it to PLL_SYS at
 * 125 MHz, which is also SysTick's input. */
const char *plat_cpu_model(void)
{
    return "Cortex-M0+ (RP2040)";
}

uint32_t plat_cpu_hz(void)
{
    return g_cpu_hz;
}
