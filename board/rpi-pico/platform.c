#include "platform.h"

/* Park-skeleton hook (issue #17).  rp2040 has no watchdog feed inside the
 * loop and no "core 1 must not return" case, so the only override is the
 * timeout line; the rest fall through to the empty defaults in rp_park.h. */
#define RP_PARK_OWNER_TIMEOUT() dbg_uart_puts("[flash] park timeout\n")

#include "rp_park.h"
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
 * (board/rpi-pico/boot2.bin) from flash offset 0, verifies a CRC-32/MPEG-2 over
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
static struct rp_park_state park = RP_PARK_STATE_INIT;
/* PARK-FIX: which core called rp_flash_park_other().  Never park the
 * initiator: it is inside the thread wait that is the only code which will
 * ever clear the flag, and once its SysTick catches the flag both cores
 * spin in the RAM loop forever. */
/* Generation counters.  The boolean only says "a request is pending"; these
 * say which one, so an ack left over from the previous request is never read
 * as this one, and a core still parked can acknowledge a new request without
 * leaving the RAM loop it is already in.  Without this, two littlefs block
 * operations back to back make the second wait for an acknowledgement the
 * first already consumed, and the write fails after the spin budget. */

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
    for (spins = 0; spins < plat_pre_tick_spins(1000000u); spins++)
        if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & RESET_PLL_SYS)
            break;

    mmio_write32(PLL_SYS_CS, 1u);                    /* REFDIV = 1        */
    mmio_write32(PLL_SYS_FBDIV, 125u);               /* 12 * 125 MHz VCO  */
    mmio_write32(PLL_SYS_PWR, 0x2du);                /* all power down    */
    mmio_write32(PLL_SYS_PWR, 0x0cu);                /* PD + VCOPD off    */
    for (spins = 0; spins < plat_pre_tick_spins(1000000u); spins++)
        if (mmio_read32(PLL_SYS_CS) & PLL_CS_LOCK)
            break;
    if (!(mmio_read32(PLL_SYS_CS) & PLL_CS_LOCK))
        return -1;

    mmio_write32(PLL_SYS_PRIM, (6u << 16) | (2u << 12)); /* /6 then /2    */
    mmio_write32(PLL_SYS_PWR, 0x04u);                /* POSTDIVPD off     */
    return 0;
}

/* Same two-case startup as the RP2350 ports (see board/rp2350-generic/
 * rp2350_xosc.h): program STARTUP.DELAY before enabling when we are the one
 * starting the crystal, otherwise (ROM already enabled it with DELAY = 0)
 * wait a full startup period after STABLE.  Leaving DELAY at 0 lets STABLE
 * assert while the frequency is still drifting; switching clk_ref onto it
 * then changes clk_sys mid-transient and can break the next flash read. */
static int xosc_wait_stable(void)
{
    for (uint32_t i = 0; i < plat_pre_tick_spins(1000000u); i++) {
        if (mmio_read32(XOSC_STATUS) & XOSC_STATUS_STABLE)
            return 1;
    }
    return 0;
}

static int xosc_start(void)
{
    uint32_t st = mmio_read32(XOSC_STATUS);

    if (!(st & XOSC_STATUS_ENABLED)) {
        mmio_write32(XOSC_STARTUP, XOSC_STARTUP_DELAY_12M);
        mmio_write32(XOSC_CTRL, XOSC_CTRL_ENABLE_12M);
        return xosc_wait_stable();
    }
    if (!(st & XOSC_STATUS_STABLE) && !xosc_wait_stable())
        return 0;
    for (volatile uint32_t d = 0; d < plat_pre_tick_spins(660000u); d++)
        ;
    return 1;
}

static void clocks_init(void)
{
    uint32_t spins;
    int locked = 0;

    if (!xosc_start())
        goto keep_ref;                   /* no crystal: stay on ROSC      */

    mmio_write32(CLOCKS_BASE + CLK_REF_CTRL_OFFSET, CLK_REF_CTRL_SRC_XOSC);
    for (spins = 0; spins < plat_pre_tick_spins(1000000u); spins++)
        if (mmio_read32(CLOCKS_BASE + CLK_REF_SELECTED_OFFSET) & (1u << 2))
            break;

    /* Start the 1 MHz tick generator inside the watchdog block - what
     * pico-sdk's runtime_init_clocks() does through start_all_ticks() before
     * main().  RP2040's ROM BOOTSEL path is watchdog based: reset_usb_boot()
     * writes the pin/interface masks to scratch[0..1] and calls
     * watchdog_reboot(wrapper, SRAM_END, 10), so its 10 ms countdown only
     * runs while this tick ticks.  With the tick stopped the ROM stops in
     * "while (true) __wfi();" and the board silently wedges instead of
     * entering BOOTSEL (pico-sdk issue #2689 is the same report).  RP2350
     * does not have this problem: its ROM uses REBOOT2 ('R','B'), which is a
     * chip-level reboot with no watchdog in the path.
     *
     * WATCHDOG_TICK: CYCLES bits 8:0, ENABLE bit 9; clk_ref is the 12 MHz
     * crystal here, so 12 gives 1 MHz.  Disarm first: a watchdog count left
     * running by the bootrom would fire the moment the tick starts (that is
     * the "reboots every ~0.5 s" this block used to avoid by leaving the tick
     * off).  plat_timer_tick() keeps re-disarming anything that arms it
     * later, and bootsel_enter_rom() stops SysTick before the ROM call so the
     * ROM's own 10 ms countdown survives. */
    mmio_write32(0x40058000u, 0u);               /* CTRL: disable, no trigger */
    mmio_write32(0x40058004u, 0x001fffffu);      /* LOAD: max, no stale count */
    mmio_write32(0x4005802cu, 12u | (1u << 9));  /* TICK: 1 MHz, running      */

    /* Park clk_sys on clk_ref (12 MHz) and wait for the glitchless mux. */
    mmio_write32(CLOCKS_BASE + CLK_SYS_DIV_OFFSET, CLK_DIV_1);
    mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET, CLK_SYS_CTRL_SRC_CLKREF);
    for (spins = 0; spins < plat_pre_tick_spins(1000000u); spins++)
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
        for (spins = 0; spins < plat_pre_tick_spins(1000000u); spins++)
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
 * ROM_TABLE_CODE('U','B') = 'U' | ('B' << 8) = 0x4255.  RP2350 has no such
 * entry - it reboots through REBOOT2 ('R','B' = 0x4252, flags BOOTSEL) - which
 * is why the two boards need different calls for the same trick.
 *
 * The RP2040 ROM implementation (pico-bootrom-rp2040, bootrom_main.c) is:
 *
 *     scratch[0] = usb_activity_gpio_pin_mask;
 *     scratch[1] = disable_interface_mask;
 *     watchdog_reboot(_usb_boot_reboot_wrapper, SRAM_END, 10);
 *     while (true) __wfi();
 *
 * Two consequences shape the code here:
 *   - the 10 ms countdown needs the watchdog's 1 MHz tick, started in
 *     clocks_init() above;
 *   - the program counter is the ROM's own wrapper, so nobody outside the
 *     bootrom can reproduce the sequence by writing scratch[4..7] and
 *     triggering the watchdog by hand.  Do not try: this file used to, and
 *     the half-written record plus a forced trigger is why Ctrl-D sometimes
 *     reset the board and still booted flash.
 *
 * So the only thing kept here is "remember the request, enter the ROM from
 * thread mode". */
typedef void (*rp_rom_usb_boot_t)(uint32_t gpio_mask, uint32_t disable_mask);
#define ROM_FUNC_RESET_USB_BOOT 0x4255u
#define BOOTSEL_MAGIC_CHAR      0x04u
#define REPLUG_CHAR             0x12u   /* Ctrl-R: fresh SE0 -> J cycle   */
#define TRACE_CHAR              0x14u   /* Ctrl-T: dump the USB IRQ trace */
#define E15_CHAR                0x05u   /* Ctrl-E: E15 workaround ON      */
#define E15_OFF_CHAR            0x17u   /* Ctrl-W: E15 workaround OFF     */

/* ROSC_CTRL.ENABLE is bits 12.., 0xfab = on.  MicroPython's
 * machine.bootloader() re-enables ROSC right before reset_usb_boot(): the
 * reset deliberately leaves ROSC/XOSC alone (PSM_WDSEL), so ROSC is the clock
 * the bootrom gets, and switching clk_sys to PLL_SYS can leave it off. */
#define ROSC_CTRL_REG           0x40060000u
#define ROSC_CTRL_ENABLE        (0x00fab000u)

/* Watchdog block.  clocks_init() touches CTRL/LOAD/TICK by address (it runs
 * before these macros are in scope); plat_reboot() uses them to do its own
 * immediate trigger, which is a plain reset and does not need the ROM. */
#define WD_BASE         0x40058000u
#define WD_CTRL_REG     (*(volatile uint32_t *)(WD_BASE + 0x00u))
#define WD_SCRATCH(n)   (*(volatile uint32_t *)(WD_BASE + 0x0cu + 4u * (n)))
#define WD_CTRL_TRIGGER 0x80000000u
#define PSM_WDSEL_REG   (*(volatile uint32_t *)(0x40010000u + 0x08u))

static volatile int g_replug_req, g_trace_req;
static volatile int g_bootsel_req;

/* The one place that talks to the ROM.  It is called from core 0's SysTick
 * handler, not from thread mode, because this kernel's idle loop is not
 * thread mode: sched_switch() enters sched_idle() from inside the SVCall, so
 * every console read made while nothing is runnable sees IPSR=0x0b.  An
 * earlier version of this file required IPSR==0 and therefore never ran -
 * measurably: Ctrl-D set the request and nothing happened.
 *
 * Exception context is fine for this call.  reset_usb_boot() only writes the
 * pin masks to scratch[0..1] and calls watchdog_reboot(wrapper, SRAM_END, 10);
 * the 10 ms countdown and the reset itself are done by the watchdog hardware,
 * not by an interrupt the exception could block.  What it does need is the
 * watchdog's 1 MHz tick (see clocks_init()) and that nothing disarms the
 * counter before it expires - hence SysTick is stopped first, since this very
 * handler disarms a watchdog it did not arm.
 *
 * The request can be set from any context (tick, panic poll, SVC console), so
 * servicing it at the top of the tick keeps it independent of where the byte
 * was seen. */
static void bootsel_enter_rom(void)
{
    rp_rom_usb_boot_t boot;

    if (!g_bootsel_req)
        return;
    g_bootsel_req = 0;
    dbg_uart_puts("[bootsel] ROM reset_usb_boot()\n");
    boot = (rp_rom_usb_boot_t)rp_rom_func(ROM_FUNC_RESET_USB_BOOT);
    if (!boot) {
        dbg_uart_puts("[bootsel] ROM lookup failed\n");
        g_bootsel_req = 1;      /* keep it: the next poll retries */
        return;
    }
    /* The bootrom runs on ROSC after the reset (PSM_WDSEL leaves it alone),
     * and clk_sys on PLL_SYS can have left it disabled. */
    mmio_write32(ROSC_CTRL_REG, ROSC_CTRL_ENABLE);
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
    boot(0u, 0u);

    /* reset_usb_boot() is documented noreturn.  If it ever returns, the tick
     * is still stopped, and without it nothing would run the scheduler again;
     * put it back.  The vector table and NVIC are deliberately left alone:
     * the ROM resets the chip through the watchdog, so it never runs from
     * them here, and on this error path the system stays usable. */
    dbg_uart_puts("[bootsel] ROM returned\n");
    mmio_write32(ARM_SYSTICK_LOAD, g_reload - 1u);
    mmio_write32(ARM_SYSTICK_VAL, 0);
    mmio_write32(ARM_SYSTICK_CTRL, 7u);
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

/* Also installed as g_panic_poll so it keeps working after the kernel dies. */
static void bootsel_poll_uart(void)
{
    int c = uart0_hw_getchar();

    if (c < 0)
        return;
    switch (c) {
    case BOOTSEL_MAGIC_CHAR:
        /* Service it here, not at one caller: this poller is reached from the
         * tick, from console output during early boot and from
         * plat_panic_halt()'s spin (g_panic_poll), and after a panic the tick
         * is stopped - so the panic loop is the only thing still running and
         * must be able to hand the chip to the ROM itself.  bootsel_enter_rom()
         * stops SysTick before the ROM arms the watchdog, so being called from
         * an interrupt or with PRIMASK set is fine. */
        g_bootsel_req = 1;
        bootsel_enter_rom();
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

/* The fault and panic reports go to UART0 only.  On the M0+ a HardFault runs
 * above every interrupt, so while the handler prints neither the USB interrupt
 * (which completes an IN transfer) nor the tick (which pumps the CDC ring) can
 * run: a CDC mirror would receive the first packet and then stall the port
 * until it was reopened.  UART0 needs no interrupt - the controller clocks the
 * bytes out - so it keeps the whole dump. */
static volatile int g_cdc_muted;

void plat_console_mirror(int on)
{
    g_cdc_muted = on ? 0 : 1;
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
    if (!g_cdc_muted)
        usb_cdc_putc(c);             /* no-op until the host enumerates */
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
    /* Only the UART paces the kernel's echo.  Asking the CDC FIFO for room
     * would deadlock the boot whenever no host is attached; the console
     * write() path uses plat_console_can_take() below instead, which answers
     * 1 when there is no host. */
    return (mmio_read32(RP_UART_FR) & RP_UART_FR_TXFF) ? 0 : 16;
}

/* Same contract as board/rp2350-generic: the CDC mirror drops whatever does
 * not fit its ring, so the console write() path has to come back short while
 * it is full and let write_all() yield - which is what gives the 1 kHz tick a
 * chance to pump the ring.  Sizing the ring to fit "one file" instead is not a
 * design: the next file is larger.  Only once the host has configured the
 * port: before that putc() drops anyway and the ring is not a sink whose room
 * means anything.  console_write_some() bounds how long a caller may keep
 * waiting on a host that never drains, so this stays a pure "is there room
 * right now" answer. */
int plat_console_can_take(int need)
{
    /* usb_cdc_configured() already means "hardware ready and the host has
     * mounted us" (g_hw_ready && tud_mounted in the driver). */
    if (usb_cdc_configured() && usb_cdc_tx_room() < need)
        return 0;
    return 1;
}

int plat_console_getchar(void)
{
    int c;

    /* bootsel_poll_uart() below may set g_bootsel_req, but the ROM call is
     * not made from here: this runs either in the tick or in the SVCall that
     * enters the idle loop, and the tick is the only place that can stop its
     * own interrupt before handing the watchdog to the ROM.  See
     * bootsel_enter_rom(). */
    c = uart0_getchar();
    if (c >= 0)
        return c;

    /* Only the driver's completed-byte ring: this function runs from
     * tty_poll(), which is called with interrupts masked (the tick and the
     * idle loop).  The state machine itself runs from the USB interrupt and
     * from the tick, never here - entering TinyUSB from a masked console read
     * is what deadlocked the other ARM port under SMP. */
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
     * RAM loop, before any instruction fetch can touch flash.  Never park
     * the core that initiated the flash call. */
    if (rp_park_should_park(&park, (int)plat_cpu_id()))
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
     * Keep it disarmed every tick.  The BOOTSEL path does not rely on this
     * timer: bootsel_enter_rom() stops SysTick before calling the ROM, so
     * the ROM's own 10 ms countdown is not cancelled here. */
    if (mmio_read32(0x40058000u) & 0x40000000u)
        mmio_write32(0x40058000u, 0u);
    g_ticks++;
    /* bootsel_poll_uart() queues every byte it harvests, so the tick cannot
     * steal keystrokes from plat_console_getchar(); it only makes Ctrl-D
     * work while the shell is blocked on a device. */
    bootsel_poll_uart();
    /* Ctrl-D is handled inside bootsel_poll_uart() (it calls
     * bootsel_enter_rom()), so the panic loop - where this tick no longer runs
     * - reaches the same code. */
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
    /* Two late snapshots, off by default: by 1 s the host has had time to
     * enumerate with interrupts unmasked, which is what the USB bring-up
     * needed to see - but they dump the whole interrupt ring and the TinyUSB
     * log buffer on every boot, and that floods the console in normal use.
     * The same dump is still one key away (TRACE_CHAR -> g_trace_req above),
     * which is how it should have been from the start.  Build with
     * -DTOS_USB_BOOT_SNAPSHOT=1 to get the automatic ones back. */
#if defined(TOS_USB_BOOT_SNAPSHOT) && TOS_USB_BOOT_SNAPSHOT
    if (g_ticks == 1000u || g_ticks == 3000u) {
        usb_dump("[usb@] ");
        usb_cdc_trace_dump();
    }
#endif
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
 * SRAM, so this loop survives core 0 turning XIP off.  The shared loop
 * re-publishes the ack for every generation: a request raised while this core
 * is still inside the loop never re-enters it, so an ack written only on
 * entry would be consumed by the earlier request. */
static void __attribute__((noinline, section(".data.flash_ram")))
rp_flash_park_loop(void)
{
    RP_PARK_PARKED_LOOP(&park, plat_pre_tick_spins(200000000u));
}

/* Called from plat_cpu_idle() on the other core.  That path is needed
 * because a core parked in sched_idle() is already inside its SysTick
 * exception and will not take another tick, so the tick-side check alone
 * can never see park.flag on an idle core. */
void plat_flash_park_poll(void)
{
    if (rp_park_should_park(&park, (int)plat_cpu_id())) {
        /* Mask interrupts BEFORE publishing the acknowledgement.  The park
         * loop tells the writer "I am in RAM now"; if anything preempts this
         * core after that point, the pre-emption resumes in flash while the
         * writer has QMI in serial command mode - a fetch that bus-faults
         * inside an exception whose handler is also in flash, i.e. a silent
         * dead core.  The tick path is already masked; only the idle and
         * spin-wait callers reach here with interrupts on. */
        uint32_t pm = plat_irq_save();

        rp_flash_park_loop();
        plat_irq_restore(pm);
    }
}

/* Mask interrupts for the handshake: with them on, the tick can land in this
 * wait loop and reschedule the caller onto the other core, which then is no
 * longer the core that will unpark - both cores park and the flag is never
 * cleared.  The shared sequence keeps the mask on success; on timeout it
 * restores and returns -1. */
static int rp_flash_park_other(void)
{
    RP_PARK_OWNER_TAKE(&park, plat_pre_tick_spins(20000000u));
}

static void rp_flash_unpark_other(void)
{
    RP_PARK_RELEASE_AND_WAKE(&park);
}
#endif

int plat_flash_exclusive_begin(void)
{
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    return rp_flash_park_other();
#else
    return 0;
#endif
}

void plat_flash_exclusive_end(void)
{
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
    rp_flash_unpark_other();
#endif
}

static int __attribute__((noinline, section(".data.flash_ram")))
rp_flash_op(uint32_t addr, const uint8_t *data, uint32_t count, int erase)
{
    const volatile uint32_t *src = (const volatile uint32_t *)XIP_BASE;
    volatile uint32_t *dst = g_boot2_copy;
    uint32_t i;

    if (plat_flash_exclusive_begin() != 0)
        return -1;

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

    plat_flash_exclusive_end();
    return 0;
}

/* ---- flash capacity: JEDEC RDID through the SSI ------------------------ */

/* After flash_exit_xip() the ROM has already put the SSI into 8-bit serial
 * command mode and left the SS pad forced high through the QSPI pad override.
 * sdk's flash_do_cmd_cs() therefore does NOT touch SSI_CTRLR0/SSIENR/SER: it
 * bit-bangs the chip select with the pad override (which wins over SSI_SER)
 * and only reads SR / writes DR0.  Doing it any other way leaves CS high, the
 * part never answers and the receive FIFO stays empty - the first attempt
 * at this hung the board that way. */
#define SSI_CTRLR0    (XIP_SSI_BASE + 0x00u)
#define SSI_SSIENR    (XIP_SSI_BASE + 0x08u)
#define SSI_RXFLR     (XIP_SSI_BASE + 0x24u)   /* receive FIFO level, in bytes */
#define SSI_SR        (XIP_SSI_BASE + 0x28u)
#define SSI_DR0       (XIP_SSI_BASE + 0x60u)
#define SSI_SR_TFNF   (1u << 1)                /* transmit FIFO not full */

/* "Has the part answered?" is asked of RXFLR, never of an SR flag bit: SR is
 * a PL022 status word whose bits shift meaning between documents (0x6 reads
 * as "TNF|RNE, a byte is waiting" or as "TNF|RFE, the FIFO is empty",
 * depending on whether bit 2 or bit 3 is the not-empty flag).  Guessing wrong
 * here cost two flash cycles - one probe that always timed out, and one
 * unbounded drain loop that wedged the boot in ramdisk_init().  RXFLR is a
 * count: zero means nothing to read, no interpretation required. */

/* Where the probe got to, in case it ever misbehaves again.  Every loop below
 * is bounded, so a silent part ends in "no measurement", never in a hung
 * boot; this counter tells us which step reached that point. */
#define PROBE_STAGE(n)   do { g_probe_dbg[7] = (n); } while (0)

/* Filled in as the probe runs and dumped by /proc/flash when the
 * measurement fails. */
/* [0] SSIENR  [1] CTRLR0  [2] SR on entry  [3] RXFLR on entry
 * [4] drain: pops<<16 | RXFLR after     [5] exchange: i<<16 | ok<<8 | RXFLR
 * [6] SR after the command              [7] last stage reached
 * [8] the four bytes read               [9] SS ctrl before  [10] after
 * [11] spare */
static volatile uint32_t g_probe_dbg[12];

#define IO_QSPI_SS_CTRL      (IO_QSPI_BASE + 0x0cu)   /* io[1] = QSPI_SS */
#define IO_QSPI_SS_OUTOVER   (3u << 8)

/* OUTOVER is the GPIO override field: 0 = drive the peripheral's value,
 * 1 = drive its inverse, 2 = force low, 3 = force high.  Only "force" values
 * actually pin the pad, and getting them wrong is invisible from the SSI
 * side: the FIFOs still shift five frames, but the part is not selected, so
 * MISO floats and the bytes are junk.  That cost a flash cycle here - with
 * 0/1 the probe reported ok=1, five bytes read, and an ID that failed the
 * 0x9f echo check. */
#define IO_QSPI_SS_LOW       (2u << 8)
#define IO_QSPI_SS_HIGH      (3u << 8)

/* Inlined into the RAM-resident probe: this runs with XIP off.  Only the
 * 2-bit OUTOVER field is touched so FUNCSEL and the pulls survive. */
static inline void rp_flash_cs_force(int high)
{
    uint32_t ctrl = mmio_read32(IO_QSPI_SS_CTRL);

    g_probe_dbg[9] = ctrl;
    ctrl = (ctrl & ~IO_QSPI_SS_OUTOVER) | (high ? IO_QSPI_SS_HIGH
                                               : IO_QSPI_SS_LOW);
    mmio_write32(IO_QSPI_SS_CTRL, ctrl);
    g_probe_dbg[10] = mmio_read32(IO_QSPI_SS_CTRL);
}

/* RDID (0x9F) is not in the ROM's flash API - flash_range_erase/program only
 * cover erase and program - so the SSI is driven by hand between
 * flash_exit_xip() and the boot2 copy, exactly the window rp_flash_op()
 * works in.  Same rules apply: this function lives in .data.flash_ram (copied
 * to SRAM by _start), and nothing it touches may be in flash while XIP is
 * off.  The result is written through `out`, which points into SRAM.
 *
 * The command is clocked as 1 + 4 bytes: the opcode, then four dummy bytes
 * that shift the 24-bit ID out.  All five are read back so the FIFO stays in
 * lockstep, and the ID lands in rx[1..3] (see below for why rx[0] is not an
 * echo).  Both polls are bounded: a quiet bus must end in "no measurement"
 * (capacity falls back to board.conf), never in a hung boot. */
void plat_flash_probe_debug(uint32_t *out)
{
    int i;

    for (i = 0; i < 12; i++)
        out[i] = g_probe_dbg[i];
}

static int __attribute__((noinline, section(".data.flash_ram")))
rp_flash_rdid(uint32_t *out, int park)
{
    const volatile uint32_t *src = (const volatile uint32_t *)XIP_BASE;
    volatile uint32_t *dst = g_boot2_copy;
    uint8_t tx[5] = {0x9fu, 0x00u, 0x00u, 0x00u, 0x00u};
    volatile uint8_t rx[5];
    uint32_t b1, b2, b3;
    uint32_t i;
    int ok = 1;

    PROBE_STAGE(1);

    if (park && plat_flash_exclusive_begin() != 0)
        return -1;

    for (i = 0; i < 64u; i++)
        dst[i] = src[i];
    __asm__ volatile("dsb\n isb" ::: "memory");

    g_rom_connect();
    g_rom_exit_xip();
    PROBE_STAGE(2);

    /* What the ROM left behind, for /proc/flash.  CTRLR0 should read 7<<16
     * (DFS_32 = 8-bit frame, TMOD = 00 transmit+receive, Motorola mode 0);
     * SSIENR should read 1. */
    g_probe_dbg[0] = mmio_read32(SSI_SSIENR);
    g_probe_dbg[1] = mmio_read32(SSI_CTRLR0);
    if (!(g_probe_dbg[0] & 1u))
        mmio_write32(SSI_SSIENR, 1u);
    g_probe_dbg[2] = mmio_read32(SSI_SR);
    g_probe_dbg[3] = mmio_read32(SSI_RXFLR) & 0xffffu;

    /* Drop whatever the ROM left in the receive FIFO, otherwise a stale byte
     * would shift the whole ID by one position.  Bounded: if the level never
     * drops we carry on and let the capacity check reject the result. */
    for (i = 0; i < 16u; i++) {
        if (mmio_read32(SSI_RXFLR) == 0u)
            break;
        (void)mmio_read32(SSI_DR0);
    }
    g_probe_dbg[4] = (i << 16u) | (mmio_read32(SSI_RXFLR) & 0xffffu);
    PROBE_STAGE(3);

    rp_flash_cs_force(0);
    PROBE_STAGE(4);

    for (i = 0; i < 5u; i++) {
        uint32_t spins = 200000u;

        while (!(mmio_read32(SSI_SR) & SSI_SR_TFNF) && --spins)
            ;
        if (!spins) { ok = 0; break; }
        mmio_write32(SSI_DR0, tx[i]);

        spins = 200000u;
        while (mmio_read32(SSI_RXFLR) == 0u && --spins)
            ;
        if (!spins) { ok = 0; break; }
        rx[i] = (uint8_t)mmio_read32(SSI_DR0);
    }
    g_probe_dbg[5] = (i << 16u) | (ok ? 0x100u : 0u) |
                     (mmio_read32(SSI_RXFLR) & 0xffu);

    rp_flash_cs_force(1);
    g_probe_dbg[6] = mmio_read32(SSI_SR);
    PROBE_STAGE(5);

    g_probe_dbg[8] = ((uint32_t)rx[0] << 24u) | ((uint32_t)rx[1] << 16u) |
                     ((uint32_t)rx[2] << 8u) | (uint32_t)rx[3];

    /* rx[0] is NOT the opcode echo.  This part only starts driving MISO once
     * the opcode has been clocked in, so the first byte read back is whatever
     * the bus idles at (measured: 0xff), and the ID lands in rx[1..3]:
     * a W25Q16 answers ff ef 40 15.  Checking rx[0] against 0x9f here once
     * rejected a perfectly good measurement - the plausibility of the ID is
     * checked by the caller instead (capacity code in 20..24, not 0 or all
     * ones), which is the check that actually means something. */

    b1 = rx[1];
    b2 = rx[2];
    b3 = rx[3];
    *out = ok ? ((b1 << 16u) | (b2 << 8u) | b3) : 0u;
    PROBE_STAGE(6);

    g_rom_flush();
    PROBE_STAGE(7);
    ((void (*)(void))((uintptr_t)g_boot2_copy + 1u))();
    __asm__ volatile("dsb\n isb" ::: "memory");
    PROBE_STAGE(8);

    if (park)
        plat_flash_exclusive_end();
    return ok ? 0 : -1;
}

/* What the part actually is, measured once.  The capacity byte is
 * log2(bytes): 20 (0x14) = 1 MiB .. 24 (0x18) = 16 MiB.  The bounds must be
 * decimal - writing 0x20 here makes 1u << code a shift of 32, undefined
 * behaviour that lets the optimiser fold the whole decode to poison (that
 * exact mistake cost a day on the RP2350 port).  Anything else is a
 * non-JEDEC part or a floating bus, and 0 means "no measurement", which
 * makes plat_lfs_blocks() fall back to the configured size. */
uint32_t plat_flash_detected_bytes(void)
{
    static uint32_t cached;
    static int done;
    uint32_t id = 0;
    uint32_t code;

    if (done)
        return cached;
    done = 1;
    if (!g_rom_ready)
        return 0;
    /* The first caller is plat_block_count(), from ramdisk_init(), which
     * kmain() runs before cpu_init() and before interrupts come on, so the
     * machine is still single-core and there is nothing to park. */
    if (rp_flash_rdid(&id, 0) != 0 || id == 0u || id == 0xffffffu)
        return 0;
    code = id & 0xffu;
    if (code < 20u || code > 24u)
        return 0;
    cached = 1u << code;
    return cached;
}

/* ---- flash unique id: command 4Bh for the USB serial ------------------ */
/* RP2350 reads its id from SYS_INFO, but RP2040 has no such register.  The
 * W25Qxx on the board answers "Read Unique ID" (4Bh): opcode + 4 dummy bytes,
 * then 8 bytes of id.  On this W25Q16JV there is NO length byte ahead of the
 * id (pico-sdk reads the same rx[5..12] window) - assuming an 08h length byte
 * silently rejected a good reading.  As with RDID, rx[0] is the idle byte,
 * not an opcode echo, and the SSI stays in the state flash_exit_xip() left it
 * in: only the pad override drives CS.
 *
 * Called from plat_console_init(), i.e. inside plat_board_early() - earlier
 * than plat_board_init(), which normally resolves the ROM flash API.  So the
 * three entries needed here are looked up on demand; plat_board_init() later
 * resolves them again harmlessly. */
static int __attribute__((noinline, section(".data.flash_ram")))
rp_flash_cmd(uint8_t op, uint32_t nframes, uint8_t *out)
{
    const volatile uint32_t *src = (const volatile uint32_t *)XIP_BASE;
    volatile uint32_t *dst = g_boot2_copy;
    uint8_t tx[20];
    uint32_t i;
    int ok = 1;

    for (i = 0; i < sizeof(tx); i++)
        tx[i] = 0u;
    tx[0] = op;

    for (i = 0; i < 64u; i++)
        dst[i] = src[i];
    __asm__ volatile("dsb\n isb" ::: "memory");

    g_rom_connect();
    g_rom_exit_xip();
    if (!(mmio_read32(SSI_SSIENR) & 1u))
        mmio_write32(SSI_SSIENR, 1u);

    for (i = 0; i < 16u; i++) {
        if (mmio_read32(SSI_RXFLR) == 0u)
            break;
        (void)mmio_read32(SSI_DR0);
    }

    rp_flash_cs_force(0);
    for (i = 0; i < nframes; i++) {
        uint32_t spins = 200000u;

        while (!(mmio_read32(SSI_SR) & SSI_SR_TFNF) && --spins)
            ;
        if (!spins) { ok = 0; break; }
        mmio_write32(SSI_DR0, tx[i]);

        spins = 200000u;
        while (mmio_read32(SSI_RXFLR) == 0u && --spins)
            ;
        if (!spins) { ok = 0; break; }
        out[i] = (uint8_t)mmio_read32(SSI_DR0);
    }
    rp_flash_cs_force(1);

    g_rom_flush();
    ((void (*)(void))((uintptr_t)g_boot2_copy + 1u))();
    __asm__ volatile("dsb\n isb" ::: "memory");
    return ok ? 0 : -1;
}

int plat_flash_unique_id(uint8_t out[8])
{
    static uint8_t cached[8];
    static int done;
    int i;

    if (done) {
        for (i = 0; i < 8; i++)
            if (cached[i])
                break;
        if (i == 8)
            return 0;
        memcpy(out, cached, 8);
        return 1;
    }
    done = 1;

    if (!g_rom_connect || !g_rom_exit_xip || !g_rom_flush) {
        g_rom_flush   = (rp_rom_flush_t)rp_rom_func(ROM_FUNC_FLASH_FLUSH_CACHE);
        g_rom_connect = (rp_rom_simple_t)rp_rom_func(ROM_FUNC_CONNECT_INTERNAL_FLASH);
        g_rom_exit_xip = (rp_rom_simple_t)rp_rom_func(ROM_FUNC_FLASH_EXIT_XIP);
    }
    if (!g_rom_connect || !g_rom_exit_xip || !g_rom_flush)
        return 0;
    {
        uint8_t raw[20];

        for (i = 0; i < sizeof(raw); i++)
            raw[i] = 0u;
        if (rp_flash_cmd(0x4bu, 13u, raw) != 0)
            return 0;
        /* Measured on this W25Q16JV: ff ff ff ff ff e6 60 9c b2 d3 4a b7 33.
         * rx[0] is the idle byte and rx[1..4] are the dummy clocks; the 64-bit
         * id starts at rx[5] with no length byte ahead of it.  The pico-sdk
         * reads the same five-plus-eight window. */
        for (i = 0; i < 8; i++)
            cached[i] = raw[5 + i];

#if TOS_DIAG
        /* Bring-up only: this line used to appear on every boot, including
         * the release image, which is exactly what boot_trace = 0 is supposed
         * to silence. */
        dbg_uart_puts("[uid]");
        for (i = 0; i < 8; i++) {
            dbg_uart_puts(" ");
            dbg_uart_hex(cached[i]);
        }
        dbg_uart_puts("\n");
#endif
    }
    for (i = 0; i < 8; i++)
        if (cached[i] != 0x00u && cached[i] != 0xffu)
            break;
    if (i == 8)
        return 0;
    memcpy(out, cached, 8);
    return 1;
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
    /* The whole littlefs partition is addressable, even though the flashed
     * image only occupies its first 300 KiB; ramdisk_init() grows the
     * filesystem over the rest (see PLAT_LFS_BLOCKS in layout.c).  The size
     * is plat_lfs_blocks(), i.e. the JEDEC capacity clamped to the board's
     * configured upper bound - not a compile-time constant. */
    return plat_lfs_blocks() * 8u;
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
    /* Use the watchdog, not SYSRESETREQ.  On this board SYSRESETREQ leaves
     * the USB/clock state such that the next boot stops after the banner,
     * and the host keeps a stale CDC node even after a later power cycle.
     * The watchdog path with PSM_WDSEL is the one BOOTSEL handover already
     * uses: it puts every peripheral back to its power-on state and lets
     * the ROM load stage-2 from flash again.  Drop D+ first so the host sees
     * a real disconnect. */
    usb_cdc_disconnect();
    for (volatile uint32_t i = 0; i < (g_cpu_hz / 3000u) * 200u; i++)
        ;

    console_shutdown();

    WD_SCRATCH(0) = 0u;
    for (unsigned i = 1; i < 8u; i++)
        WD_SCRATCH(i) = 0u;
    PSM_WDSEL_REG = 0x0001fffcu;
    WD_CTRL_REG = WD_CTRL_TRIGGER;

    for (;;)
        ;
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

/* No MPU and no MMU: a stack overflow is a silent write into the same window,
 * not a fault.  Two things stand in for the missing protection:
 *
 *   - plat_user_stack_guard(): a canary word under the stack, checked on every
 *     syscall, so an overflow is detected and either grown into the heap's
 *     tail or turned into a kill with a message;
 *   - the reserve below: one 4 KiB step for a task that did not declare its
 *     stack, which is what that growth consumes.
 *
 * A window can therefore be the same shape as the C3's - 4 KiB steps, 4 KiB
 * default stack - without losing the growth path. */
uint32_t plat_window_grow_reserve(void)
{
    return PLAT_STACK_BYTES;
}

int plat_user_stack_guard(void)
{
    return 1;
}

/* Nothing here encodes a region, so the stack does not have to divide the
 * grain (see kernel/process.c). */
int plat_window_needs_stack_alignment(void)
{
    return 0;
}
