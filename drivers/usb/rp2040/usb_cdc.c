/* USB CDC-ACM console for the RP2040, built on TinyUSB.
 *
 * The RP2350 port is the template: the same SIE and the same TinyUSB device
 * controller driver, so the protocol layer is delegated to TinyUSB and this
 * file only owns what is chip specific:
 *
 *   - PLL_USB -> 48 MHz on clk_usb.  The RP2040 PLL is a plain integer
 *     PLL_USB at 0x4002c000, and clk_usb is CLOCKS_CLK_USB_CTRL/DIV at
 *     offsets 0x54/0x58 (the RP2350 moved them to 0x60/0x64).
 *   - RESETS_USBCTRL is bit 24 here (bit 28 on the RP2350).
 *   - No pad mux at all: the RP2040 has an internal USB PHY on dedicated
 *     pins, so usb_pads_setup() is empty.
 *   - USBCTRL is NVIC IRQ 5 (IRQ 14 on the RP2350); see USBCTRL_IRQ in
 *     third_party/tinyusb/port_rp2.
 *
 * Threading model: USBCTRL runs TinyUSB's dcd_int_handler, which does
 * nothing but timestamp events into a queue.  All the actual work happens in
 * tud_task(), called from usb_cdc_poll() on every 1 ms tick and from the
 * console API - the same split pico-sdk uses for stdio_usb, which drives
 * tud_task from a 1 ms repeating timer.
 *
 * Configuration lives in drivers/usb/rp2040/tusb_config.h; the pico-sdk
 * facade the upstream port needs is in third_party/tinyusb/port_rp2 and
 * tusb_shim.c.
 */

#include "platform.h"
#include "board.h"
#include "arm_memmap.h"
#include "scs.h"
#include "usb_cdc.h"

#include "tusb.h"
#include "device/usbd_pvt.h"


#include "device/dcd.h"
#include "hardware/structs/usb.h"
#include "string.h"
#include "console.h"
#include "uart_dbg.h"

/* Fills g_serial from the flash part's unique id; defined with the
 * descriptors below.  Every board used to send "0001", which made two
 * RP2040s on one host look like one device. */
static void serial_from_chip_id(void);

/* SIO hardware spinlock 30 (0xD0000000 + 0x100 + 4*30).  31 is the kernel's
 * spin gate on the SMP-capable ARM port.  Defined at the top because
 * usb_cdc_init() force-releases it before anything else can take it - the
 * SIO block survives a warm reboot and a previous run may have died holding
 * the lock. */
#define USB_SIO_SPINLOCK (*(volatile uint32_t *)(0xD0000000u + 0x100u + 4u * 30u))

/* TinyUSB declares this weak and calls it only when the symbol is non-NULL:
 *
 *     if (usbd_app_driver_get_cb) _app_driver = usbd_app_driver_get_cb(...);
 *
 * An *undefined* weak symbol does not materialise as 0 on every port.  On
 * RV32 the medany code model makes the reference PC-relative, lld hands back
 * a non-zero address for it, the NULL test passes and the call jumps into
 * nothing - which is what kept the RP2350 RISC-V image from booting unless
 * the ROM had already brought USB up.  A definition that reports "no
 * application driver" gives the same answer on every target; it stays weak
 * so an application can still replace it.
 */
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    (void)driver_count;
    return 0;
}

/* The port header defines this pair; the bring-up path uses both to
 * re-assert the pull-up after the pads come up. */
#define usb_hw_set   ((usb_hw_t *)hw_set_alias_untyped(usb_hw))
#define usb_hw_clear ((usb_hw_t *)hw_clear_alias_untyped(usb_hw))

/* ---- reset / clock registers ------------------------------------------- */

#define RESETS_PLL_USB    0x00002000u

#define PLL_USB_BASE      0x4002c000u   /* RP2040 PLL_USB                   */
#define PLL_CS(off)       (*(volatile uint32_t *)(PLL_USB_BASE + (off)))
#define PLL_CS_REG        PLL_CS(0x00)  /* LOCK = bit 31, REFDIV = 5:0      */
#define PLL_PWR_REG       PLL_CS(0x04)
#define PLL_FBDIV_REG     PLL_CS(0x08)
#define PLL_PRIM_REG      PLL_CS(0x0c)  /* POSTDIV1 = 18:16, POSTDIV2 = 14:12 */

#define PLL_CS_LOCK       0x80000000u
#define PLL_PWR_PD        0x00000001u
#define PLL_PWR_DSMPD     0x00000004u   /* 1 = integer mode, leave it set   */
#define PLL_PWR_POSTDIVPD 0x00000008u
#define PLL_PWR_VCOPD     0x00000020u

/* clk_usb must be exactly 48 MHz.  12 MHz XOSC / 1 * 100 = 1200 MHz VCO
 * (inside the 750-1600 MHz range), / 5 / 5 = 48 MHz. */
#define PLL_REFDIV        1u
#define PLL_FBDIV         100u
#define PLL_POSTDIV1      5u
#define PLL_POSTDIV2      5u

/* RP2040: CLK_USB_CTRL at 0x54 holds ENABLE (bit 11) + AUXSRC (7:5).
 * CLK_USB_DIV at 0x58 is a *2-bit* integer divider: INT is bits 9:8, with
 * no fractional part, and the reset value 0x00000100 is already /1.
 * (19:16 belongs to the RP2350 / is simply reserved here - writing 1<<16
 * leaves INT at 0, which gates clk_usb off and keeps USBCTRL in reset
 * forever: no CDC enumeration, and the ROM's USB BOOTSEL cannot start.) */
#define CLOCKS_CLK_USB_CTRL (*(volatile uint32_t *)(0x40008000u + 0x54u))
#define CLOCKS_CLK_USB_DIV  (*(volatile uint32_t *)(0x40008000u + 0x58u))
#define CLK_USB_ENABLE      0x00000800u
#define CLK_USB_DIV_INT_1   0x00000100u
#define CLK_USB_AUX_PLL_USB 0x00000000u

/* ---- state -------------------------------------------------------------- */

static int g_hw_ready;          /* clocks + controller up                 */
static int g_init_stage;        /* 0 ok, 1 reset timeout, 2 PLL timeout   */
static uint32_t g_pll_cs_boot;  /* PLL_CS as the ROM handed it over       */
static uint32_t g_ss_pads;      /* SIE_STATUS right after the pad writes  */
static uint32_t g_ss_retrig;    /* ... and after the pull-up retrigger    */

/* Bring-up counters: they tell a frozen TinyUSB state machine (g_in_task
 * stuck at 1) apart from a USB interrupt that is never taken. */
static volatile uint32_t g_task_calls;      /* tud_task() from poll     */
static volatile uint32_t g_task_isr_calls;  /* tud_task_ext() from IRQ   */
static volatile int g_in_task;

/* A tiny RAM trace of the first few USBCTRL interrupts.  Formatting inside
 * the IRQ would cost ~87 us per character at 115200 and destroy exactly the
 * enumeration timing we are measuring, so the ISR only snapshots registers
 * and the tick (or Ctrl-T) prints them later. */
#define USB_TRACE_N 8u
struct usb_trace_ent {
    uint32_t ints, raw, sie, buf;
    uint32_t setup0, setup1;    /* DPRAM 0x100: the 8-byte SETUP packet   */
    uint32_t ep0in, ep0out;     /* DPRAM 0x080/0x084: EP0 buffer control  */
    uint32_t mnt;
};
static struct usb_trace_ent g_usb_trace[USB_TRACE_N];
static volatile uint32_t g_usb_trace_n;

/* TinyUSB's level-2 log has to be captured in RAM, not printed live: writing
 * one character to UART0 costs 87 us at 115200, and the log is produced from
 * inside the USB interrupt, so printing it would destroy the very timing we
 * are trying to measure.  Keep the FIRST kilobyte (init, bus reset, SETUP,
 * descriptor handling) and drop the rest. */
#define USB_LOG_SIZE 1024u
static char g_usb_log[USB_LOG_SIZE];
static volatile unsigned g_usb_log_len;

void usb_cdc_irq_enable(void);  /* defined below, needed during init      */

/* ---- RP2040-E15 enumeration workaround ---------------------------------- */

/* The chip reports "Chip Version B1".  pico-sdk carries a workaround for
 * erratum RP2040-E15: coming out of reset the SIE wants 800 us of LS_J
 * before it enters the connected state; on a hub that broadcasts traffic
 * for other devices that never happens and the host's SETUP packets are
 * ignored entirely - exactly the BUS_RESET + DEV_SUSPEND trace with zero
 * SETUP_REQ this port produces.  TinyUSB would call the SDK fix from
 * dcd_int_handler(), but the vendored port has TUD_OPT_..._FIX off and the
 * shim header even says "the target is an RP2350".
 *
 * Default ON: this is a B1 part and the measured symptom matches the
 * erratum verbatim.  Ctrl-W turns it off for the A/B, Ctrl-E back on. */
static volatile int g_e15_fix = 1;
static volatile int g_e15_req;
static volatile int g_e15_busy;
static volatile uint32_t g_e15_calls;
static uint32_t g_usb_clk_hz = 12000000u;

uint32_t usb_cdc_e15_calls(void)
{
    return g_e15_calls;
}

/* Called after a runtime clk_sys change so the E15 forced-J hold stays
 * ~1 ms instead of shrinking with the faster CPU. */
void usb_cdc_set_cpu_hz(uint32_t hz)
{
    g_usb_clk_hz = hz;
}

#define E15_GPIO15_CTRL  (*(volatile uint32_t *)(0x40014000u + 0x7cu))
#define E15_GPIO15_PAD   (*(volatile uint32_t *)(0x4001c000u + 0x40u))
#define E15_MUXING       (*(volatile uint32_t *)(USBCTRL_REGS_BASE + 0x074u))
#define E15_PHY_DIRECT   (*(volatile uint32_t *)(USBCTRL_REGS_BASE + 0x07cu))
#define E15_PHY_OVERRIDE (*(volatile uint32_t *)(USBCTRL_REGS_BASE + 0x080u))
#define E15_SIE_STATUS   (*(volatile uint32_t *)(USBCTRL_REGS_BASE + 0x050u))

#define E15_LINE_STATE_J    1u
#define E15_SIE_CONNECTED   0x00010000u
#define E15_MUX_DIGITAL     (0x4u | 0x8u)   /* TO_DIGITAL_PAD | SOFTCON   */
#define E15_MUX_PHY         (0x1u | 0x8u)   /* TO_PHY          | SOFTCON   */

void usb_cdc_set_e15(int on)
{
    g_e15_fix = on ? 1 : 0;
}

int usb_cdc_e15_pending(void)
{
    return g_e15_req || g_e15_busy;
}

/* Ask the poll path to run the erratum workaround; never blocks the USB
 * interrupt.  The reset itself starts with 10-15 ms of SE0, so waiting for
 * that inside the IRQ (the first version) stalled every USB event for the
 * whole reset - exactly the window in which the host starts SETUP. */
void usb_cdc_e15_request(void)
{
    g_e15_req = 1;
}

/* Busy-wait `ms` milliseconds at the current clk_sys.  The USB bring-up is
 * the only caller and it runs before the 1 ms tick exists, so this cannot use
 * plat_uptime_ms(). */
static void usb_delay_ms(uint32_t ms)
{
    /* One iteration of this loop measures ~9.2 cycles on this port (the
     * pull-up drop below was timed against the UART: 200 nominal took 614 ms
     * at 125 MHz with the old /3000 divisor, which assumed 3 cycles).  /9000
     * puts the real duration back on the requested one - 200 ms for the
     * disconnect a hub needs to tear the old device node down, ~1 ms for the
     * E15 forced-J hold the SIE wants to see at least 800 us of. */
    uint32_t n = (g_usb_clk_hz / 9000u) * ms;

    if (n < 1200u * ms)
        n = 1200u * ms;
    for (volatile uint32_t i = 0; i < n; i++)
        ;
}

/* Service one pending BUS_RESET.  Called from usb_cdc_poll(), i.e. thread
 * context during the early enumeration wait and from the 1 ms tick after
 * boot - never from the USB IRQ.  Mirrors pico-sdk's alarm-driven
 * rp2040_usb_device_enumeration.c: wait for SE0 to end, force LS_J through
 * GPIO15 for 1 ms, wait for SIE_STATUS.CONNECTED, then restore the PHY. */
void usb_cdc_e15_service(void)
{
    uint32_t ctrl_prev, pad_prev, spins;

    if (!g_e15_req)
        return;
    g_e15_req = 0;
    if (!g_e15_fix || !g_hw_ready)
        return;
    if (!(usb_hw->sie_ctrl & USB_SIE_CTRL_PULLUP_EN_BITS))
        return;
    g_e15_calls++;
    g_e15_busy = 1;

    /* 1. Wait for the host to stop asserting SE0, bounded at ~40 ms. */
    spins = (g_usb_clk_hz / 3000u) * 40u;
    if (spins < 40000u)
        spins = 40000u;
    for (uint32_t i = 0; i < spins; i++)
        if (((E15_SIE_STATUS >> 2) & 3u) != 0u)
            break;

    /* 2. Force LS_J on the SIE input path via GPIO15's USB debug mux.
     * DM stays 0 because GPIO16 is not funcsel'd to function 8. */
    ctrl_prev = E15_GPIO15_CTRL;
    pad_prev  = E15_GPIO15_PAD;

    E15_GPIO15_PAD   |= (1u << 3) | (1u << 2);      /* PUE | PDE (bus keep) */
    E15_GPIO15_CTRL   = (E15_GPIO15_CTRL & ~0x00003000u) | (2u << 12); /* OEOVER=LOW */
    E15_GPIO15_CTRL   = (E15_GPIO15_CTRL & ~0x0000001fu) | 8u;         /* FUNCSEL=8  */
    E15_GPIO15_CTRL   = (E15_GPIO15_CTRL & ~0x00030000u) | (3u << 16); /* INOVER=HIGH*/
    E15_PHY_DIRECT   |= 0x2u;                       /* DP_PULLUP_EN         */
    E15_PHY_OVERRIDE |= 0x4u;                       /* ..._OVERRIDE_EN      */
    E15_MUXING        = E15_MUX_DIGITAL;

    /* 3. Hold J for ~1 ms. */
    usb_delay_ms(1);

    /* 4. The SIE must now report CONNECTED before we hand the pins back,
     * otherwise the whole exercise was invisible to it. */
    spins = (g_usb_clk_hz / 3000u) * 10u;
    if (spins < 10000u)
        spins = 10000u;
    for (uint32_t i = 0; i < spins; i++)
        if (E15_SIE_STATUS & E15_SIE_CONNECTED)
            break;

    /* 5. Restore, in the SDK's order: PHY mux -> remove the pull-up
     * override -> GPIO15 ctrl -> GPIO15 pad. */
    E15_MUXING        = E15_MUX_PHY;
    E15_PHY_OVERRIDE &= ~0x4u;
    E15_GPIO15_CTRL   = ctrl_prev;
    E15_GPIO15_PAD    = pad_prev;
    g_e15_busy = 0;
}

/* ---- PHY pads ----------------------------------------------------------- */

/* RP2040: the USB PHY is on dedicated pins (USB_DP/DM) and there is no pad
 * mux to program, unlike the RP2350 where GPIO46/47 have to be switched to
 * function 31 and their ISO bits cleared.  The hook stays so the bring-up
 * sequence below is identical on both chips. */
static void usb_pads_setup(void)
{
}

/* ---- clock and reset bring-up ------------------------------------------- */

/* Returns 0 on success.  Every wait is bounded: an unresponsive block has to
 * leave the UART console usable instead of hanging the kernel before the
 * shell ever starts. */
static int usb_clock_init(void)
{
    uint32_t m, spins;

    g_pll_cs_boot = PLL_CS_REG;

    /* Bring PLL_USB and USBCTRL out of reset.  RESET_DONE is *not* polled
     * yet: the USBCTRL done bit stays low until clk_usb is actually running
     * (the RP2350 reads done=0x06c286c0 with bit 28 clear), so waiting here
     * would time out on real hardware. */
    m = mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET);
    m &= ~(RESETS_USBCTRL | RESETS_PLL_USB);
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET, m);

    /* The PLL registers are ignored while PLL_USB is still coming out of
     * reset.  Wait for its RESET_DONE bit before writing CS/FBDIV; USBCTRL
     * is excluded because its done bit only rises after clk_usb runs. */
    for (spins = 0; spins < plat_pre_tick_spins(1000000u); spins++)
        if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) &
            RESETS_PLL_USB)
            break;
    if (spins == plat_pre_tick_spins(1000000u)) {
        g_init_stage = 2;
        return -1;
    }

    /* PLL_USB follows pico-sdk's pll_init() literally.  The RP2040-style
     * one-shot "PWR = 0" is fatal here: it clears DSMPD (enabling the
     * delta-sigma modulator in an integer-FBDIV setup) and releases
     * POSTDIVPD before the VCO has ever locked.  The ROM may also have left
     * the PLL locked from its USB BOOTSEL path, in which case the dividers
     * must not be touched at all. */
    if (!(PLL_CS_REG & PLL_CS_LOCK)) {
        /* A PLL left in a bad state needs a real reset/unreset cycle, not
         * just a deassert. */
        m = mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET);
        mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET, m | RESETS_PLL_USB);
        mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET, m & ~RESETS_PLL_USB);
        for (spins = 0; spins < plat_pre_tick_spins(1000000u); spins++) {
            if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & RESETS_PLL_USB)
                break;
        }

        PLL_CS_REG = PLL_REFDIV;        /* 12 MHz * 100 = 1200 MHz VCO      */
        PLL_FBDIV_REG = PLL_FBDIV;

        /* Only PD and VCOPD come off here; DSMPD and POSTDIVPD stay set. */
        PLL_PWR_REG &= ~(PLL_PWR_PD | PLL_PWR_VCOPD);
        for (spins = 0; spins < plat_pre_tick_spins(1000000u); spins++) {
            if (PLL_CS_REG & PLL_CS_LOCK)
                break;
        }
        if (spins == plat_pre_tick_spins(1000000u)) {
            g_init_stage = 2;
            return -1;                  /* PLL never locked: UART only      */
        }

        /* Post dividers only after the VCO is stable: /5/5 = 48 MHz. */
        PLL_PRIM_REG = (PLL_POSTDIV1 << 16) | (PLL_POSTDIV2 << 12);
        PLL_PWR_REG &= ~PLL_PWR_POSTDIVPD;
    }

    CLOCKS_CLK_USB_DIV = CLK_USB_DIV_INT_1;
    CLOCKS_CLK_USB_CTRL = CLK_USB_ENABLE | CLK_USB_AUX_PLL_USB;

    /* Now clk_usb is running, so USBCTRL can finish coming out of reset. */
    for (spins = 0; spins < plat_pre_tick_spins(1000000u); spins++) {
        if ((mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) &
             (RESETS_USBCTRL | RESETS_PLL_USB)) ==
            (RESETS_USBCTRL | RESETS_PLL_USB))
            break;
    }
    if (spins == plat_pre_tick_spins(1000000u)) {
        g_init_stage = 1;
        return -1;                      /* no USB: UART-only console        */
    }
    return 0;
}

/* ---- bring-up ----------------------------------------------------------- */

/* Filled in by usb_cdc_init() and read by /proc/usbboot. */
extern volatile uint32_t g_usb_boot[8];

void usb_cdc_init(void)
{
    g_hw_ready = 0;
    g_init_stage = 0;
    serial_from_chip_id();

    /* /proc/usbboot: what the boot path left behind.  USBCTRL is released
     * only by a boot path that used it - the BOOTSEL mass-storage ROM - so
     * this one bit separates "the host is still holding the device node from
     * before" from "the cable was never enumerated".  It is the candidate
     * signal for skipping the 200 ms D+ drop on cold boots (OPEN-ITEMS 3). */
    g_usb_boot[0] = mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET);
    g_usb_boot[1] = (g_usb_boot[0] & RESETS_USBCTRL) ? 0u : 1u;

    /* A warm boot can land with the cross-core USB spinlock still held by
     * the previous run - the SIO block survives a core reset.  This is the
     * first touch, before either core can take the lock again. */
    USB_SIO_SPINLOCK = 0;

    /* Order matters: PLL and clk_usb first (the controller's reset-done bit
     * depends on the clock), then the pads, then TinyUSB - whose dcd_init()
     * resets the controller itself and would undo any register-level setup
     * done before it. */
    if (usb_clock_init() != 0)
        return;

    usb_pads_setup();

    /* Mark ready before TinyUSB can raise an interrupt: the handler bails
     * out on !g_hw_ready, and an interrupt that is never drained keeps
     * re-asserting, which is how an earlier build spun forever. */
    g_hw_ready = 1;

    if (!tud_init(0)) {
        g_hw_ready = 0;
        return;
    }

    /* And again, now that TinyUSB's dcd_init() has reset the controller and
     * cleared MAIN_CTRL.PHY_ISO.  The same writes issued *before* that reset
     * do not take effect: measured on hardware, pads written ahead of
     * tud_init() leave SIE_STATUS at VBUS-only (SE0), while writing them
     * after it turns the bus into VBUS|J|CONNECTED.  The controller reset is
     * the only step in between, so the pad write has to follow it. */
    usb_pads_setup();
    g_ss_pads = usb_hw->sie_status;
    g_usb_boot[2] = usb_hw->sie_status;
    g_usb_boot[3] = usb_hw->sie_ctrl;

    /* The pull-up was raised inside tud_init(), before the pad's input buffer
     * existed.  Drop and re-raise it so the connect is detected against the
     * configured pad instead of an isolated one - the same sequence that
     * turned SIE_STATUS from 0x00000001 into 0x00010005 when it was tried by
     * hand after boot. */
    usb_hw_clear->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS;
    /* The host has to SEE the disconnect.  200 ms is what a hub needs to
     * tear down the previous device node; after a UF2 flash the ROM's MSC
     * may have left D+ pulled up, so a host that never observes SE0 keeps
     * talking to the old device and our CDC is never enumerated. */
    usb_delay_ms(200);
    g_usb_boot[6] = 1u;                     /* the D+ drop ran ...       */
    g_usb_boot[7] = usb_hw->sie_status;     /* ... and the bus went idle */
    usb_hw_set->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS;
    usb_delay_ms(5);
    g_ss_retrig = usb_hw->sie_status;
    g_usb_boot[4] = g_ss_pads;
    g_usb_boot[5] = g_ss_retrig;

    /* The NVIC gate must be open BEFORE usb_cdc_console_init() starts its
     * bounded enumeration wait: at that point the 1 ms tick does not exist
     * yet, so without the interrupt TinyUSB never receives a single event
     * and tud_mounted() can never become true.  Gating this on "already
     * configured" is a deadlock. */
    usb_cdc_irq_enable();
}

/* Open the NVIC gate for IRQ 5.  Also called by the board layer; harmless
 * to repeat. */
void usb_cdc_irq_enable(void)
{
    if (!g_hw_ready)
        return;
    dcd_int_enable(0);
}

/* The console does not wait for the host.

 * It used to spin - nominally 1.5 s, in practice 8-10 s because each
 * iteration also runs tud_task() - until SET_CONFIGURATION arrived, so that
 * the boot banner would not be written into a FIFO nobody reads.  Measured
 * on the RP2040: the board sat with the LED on for 8-10 s after every reset,
 * and the CDC port only appeared when that loop finally gave up, so the wait
 * delayed the very enumeration it was waiting for rather than observing it.
 *
 * Nothing is lost by not waiting: the boot log the host misses is exactly
 * what it missed before, and the port is configured as soon as the host
 * shows up.  usb_cdc_putc() keeps its "drop until mounted" guard - buffering
 * there instead made the RP2040's boot stall on the UART as well. */

/* Pump the controller for a short, bounded time right after init, so the
 * host's first control transfers are answered during boot instead of timing
 * out and backing off.  Same shape and the same cycle window as the RP2350
 * port (see drivers/usb/rp2350/usb_cdc.c): 40 000 polls at 125 MHz is ~250 ms,
 * derived from the running clock so the 12 MHz crystal-failure path does not
 * stretch it.  Stops as soon as the port is configured. */
static void usb_cdc_boot_pump(void)
{
    uint32_t n = plat_cpu_hz() / 3125u;      /* 125 MHz -> 40 000, ~250 ms */

    if (n > 200000u)
        n = 200000u;
    if (n < 4000u)
        n = 4000u;
    for (uint32_t i = 0; i < n && !tud_mounted(); i++)
        usb_cdc_poll();
}

void usb_cdc_console_init(uint32_t clk_hz)
{
    /* The E15 workaround holds LS_J for ~1 ms with a volatile loop; it
     * needs the runtime clk_sys (which the Ctrl-Y / Ctrl-U A/B changes). */
    g_usb_clk_hz = clk_hz;
    usb_cdc_init();
    usb_cdc_boot_pump();
}

/* ---- cross-core USB exclusion ------------------------------------------- */

/* TinyUSB state used to be guarded by PRIMASK only - per core.  On an SMP
 * build (rpi-pico-smp) the console writer on one core raced the other core's
 * tud_task()/write_flush/dcd handler, corrupting tu_fifo bookkeeping; the
 * same failure wedged atk-dnr2350am-smp's idle core forever inside tty_poll.  The
 * SIO hardware spinlock is cross-core by construction, and PRIMASK around
 * it keeps this core's USB interrupt from taking the lock again on a core
 * that already holds it (the ISR pre-empts only unmasked code, and a lock
 * holder is always masked).  Hold times are bounded: tud_task() drains the
 * event queue and returns; nothing inside waits on the other core. */
/* Token bits, same shape as the RP2350 port (USB_LK_FAILED is public):
 *   bit31 - nested call on the same core, the outer section holds the lock
 *   bit30 - no spinlock was offered at boot: run without one (reported)
 *   bit29 - the bounded acquire gave up: the caller must skip the work */
#define USB_LK_NESTED 0x80000000u
#define USB_LK_NONE   0x40000000u
/* ~20 ms at 125 MHz: long enough for any legitimate hold (the task drains a
 * bounded event queue and returns), short enough that a wedge is not a dead
 * board.  The acquire used to spin with no bound at all. */
#define USB_LK_SPINS 300000u

static volatile uint32_t usb_depth[2];
volatile uint32_t        g_usb_lock_fail;

/* Live counters for /proc/usbboot; see kernel/procfs.c for the index. */
extern volatile uint32_t g_usb_stat[4];

/* Any non-zero answer means "acquired".  RP2040 answers 1, and RP2350
 * answers one-hot (bit 30 for lock 30) - testing bit 31 here made the lock
 * look permanently held on hardware and wedged boot. */
static inline int usb_lk_claim(void)
{
    if (USB_SIO_SPINLOCK == 0u)
        return 0;
    __asm__ volatile("dmb" ::: "memory");
    return 1;
}

uint32_t usb_cdc_trylock(void)
{
    uint32_t id = (uint32_t)plat_cpu_id() & 1u;
    uint32_t pm;

    /* Nested on the same core: the hardware lock is not recursive, and the
     * outer section already masked interrupts. */
    if (usb_depth[id]) {
        usb_depth[id]++;
        return USB_LK_NESTED;
    }
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(pm) :: "memory");
    if (!usb_lk_claim())
        return pm | USB_LK_FAILED;
    usb_depth[id] = 1;
    return pm;
}

uint32_t usb_cdc_lock(void)
{
    uint32_t id = (uint32_t)plat_cpu_id() & 1u;
    uint32_t pm;
    uint32_t n;

    if (usb_depth[id]) {
        usb_depth[id]++;
        return USB_LK_NESTED;
    }
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(pm) :: "memory");
    for (n = 0; ; n++) {
        if (usb_lk_claim()) {
            usb_depth[id] = 1;
            return pm;
        }
        if (n >= USB_LK_SPINS) {
            /* Give up on this section only.  Dropping the lock for the rest
             * of the boot - what this used to do - turned one wedge into
             * silent cross-core FIFO corruption. */
            g_usb_stat[0] = ++g_usb_lock_fail;
            return pm | USB_LK_FAILED;
        }
    }
}

void usb_cdc_unlock(uint32_t tok)
{
    uint32_t id = (uint32_t)plat_cpu_id() & 1u;

    if (tok & USB_LK_NESTED) {
        if (usb_depth[id])
            usb_depth[id]--;
        return;
    }
    if (tok & (USB_LK_NONE | USB_LK_FAILED)) {
        __asm__ volatile("msr primask, %0" :: "r"(tok & 1u) : "memory");
        return;
    }
    usb_depth[id] = 0;
    __asm__ volatile("dmb" ::: "memory");
    USB_SIO_SPINLOCK = 0;
    __asm__ volatile("msr primask, %0" :: "r"(tok & 1u) : "memory");
}

/* ---- event pump --------------------------------------------------------- */

/* TinyUSB splits the work in two: dcd_int_handler() timestamps what the SIE
 * reported, and tud_task() runs the state machine that answers the host.
 * pico-sdk calls tud_task() straight from the USB interrupt for exactly the
 * reason measured here: with the task deferred to the 1 ms tick, every
 * control-transfer stage was answered on the next millisecond, the host ran
 * out of patience after the first descriptor and reset the port in a loop.
 * So the interrupt path runs both, with a guard against re-entering the task
 * when the interrupt lands inside the tick's own tud_task() call. */
/* Bytes the state machine has completed, waiting for the console.
 *
 * tud_cdc_read_char() is TinyUSB code, and the console poll reaches the read
 * path with interrupts masked (tty_poll runs in the tick and in sched_idle).
 * Nothing in that path may wait or take a cross-core lock: the other core can
 * hold the USB gate for a whole state-machine pass, and the masked core would
 * stop ticking while it waits (the ARM SMP console deadlock).  So the task -
 * which already runs here, under the lock, on core 0 - moves the bytes out
 * into this ring, and the masked reader only pops a byte array.
 *
 * Both sides run on core 0: the USB interrupt is only enabled in core 0's
 * NVIC, tty_poll is called from core 0's tick and idle loop.  The pop runs
 * masked, so the interrupt cannot preempt it.  That makes this a single-core
 * producer/consumer pair with no atomics needed.
 */
#define USB_RX_Q 256
static volatile uint8_t  usb_rxq[USB_RX_Q];
static volatile uint16_t usb_rxq_head, usb_rxq_tail;   /* head = next write */

static void usb_rxq_fill(void)
{
    int32_t c;

    /* Look before reading: tud_cdc_read_char() consumes, so checking for a
     * full ring after it has returned a byte would drop that byte.  A full
     * ring leaves the rest in TinyUSB, where the next pass picks it up. */
    while (tud_cdc_available() > 0) {
        uint16_t n = (uint16_t)((usb_rxq_head + 1u) % USB_RX_Q);

        if (n == usb_rxq_tail)
            return;                 /* full: the rest stays in TinyUSB */
        c = tud_cdc_read_char();
        if (c < 0)
            return;
        usb_rxq[usb_rxq_head] = (uint8_t)c;
        usb_rxq_head = n;
    }
}

/* Bytes the console has written, waiting for the state machine.
 *
 * putc is called from the tick, from the echo path and from fault handlers -
 * always with interrupts masked.  It must not take the USB lock (the other
 * core can hold it for a whole state-machine pass - the ARM SMP console
 * deadlock) and it must not enter TinyUSB.  Each core appends to its own ring;
 * the pump, under the lock on core 0, is the only reader.
 *
 * The head is published with a barrier after the byte is stored, and the
 * drain works from a snapshot, so a producer that keeps writing - including
 * putc called from inside the pump - only ever defers bytes to the next
 * pass. */
#if defined(TOS_MAX_CPUS) && TOS_MAX_CPUS > 1
#define USB_TX_CORES 2
#else
#define USB_TX_CORES 1
#endif
#define USB_TX_Q 256
static volatile uint8_t  usb_txq[USB_TX_CORES][USB_TX_Q];
static volatile uint16_t usb_txq_head[USB_TX_CORES], usb_txq_tail[USB_TX_CORES];

/* 1 = stored, 0 = the ring is full. */
static int usb_txq_put(uint32_t id, uint8_t c)
{
    uint16_t h, n;

    if (id >= USB_TX_CORES)
        id = 0;                         /* a core with no ring of its own */
    h = usb_txq_head[id];
    n = (uint16_t)((h + 1u) % USB_TX_Q);

    if (n == usb_txq_tail[id])
        return 0;
    usb_txq[id][h] = c;
    __asm__ volatile("dmb" ::: "memory");   /* the byte before the head */
    usb_txq_head[id] = n;
    return 1;
}

/* Move queued bytes into TinyUSB.  Lock held, core 0.  tud_cdc_write_flush()
 * hands what is queued to the endpoint and never waits for the host, so this
 * stays bounded. */
static void usb_txq_drain(void)
{
    uint32_t moved = 0;

    for (uint32_t id = 0; id < USB_TX_CORES; id++) {
        uint16_t h = usb_txq_head[id];          /* snapshot */

        __asm__ volatile("dmb" ::: "memory");
        while (usb_txq_tail[id] != h) {
            if (!tud_cdc_write_available()) {
                tud_cdc_write_flush();
                if (!tud_cdc_write_available())
                    break;                      /* host behind: keep the rest */
            }
            tud_cdc_write_char((char)usb_txq[id][usb_txq_tail[id]]);
            usb_txq_tail[id] = (uint16_t)((usb_txq_tail[id] + 1u) % USB_TX_Q);
            moved++;
        }
    }
    if (moved || tud_cdc_write_available() < CFG_TUD_CDC_TX_BUFSIZE)
        tud_cdc_write_flush();
    g_usb_stat[3] += moved;
}

static void usb_task_run(int from_isr)
{
    uint32_t primask = usb_cdc_lock();

    if (g_in_task || !USB_LK_OK(primask)) {
        usb_cdc_unlock(primask);
        return;                     /* outer call, or no lock this pass */
    }
    g_in_task = 1;

    if (from_isr) {
        g_task_isr_calls++;
        tud_task_ext(0, true);
    } else {
        g_task_calls++;
        tud_task();
    }

    usb_rxq_fill();
    usb_txq_drain();

    g_in_task = 0;
    usb_cdc_unlock(primask);
}

/* IRQ 5, called from the shared vector table through g_usb_irq_handler.
 * dcd_int_handler is TinyUSB's own; it has already been registered by
 * dcd_init() and is invoked by the trampoline in tusb_shim.c, which then
 * calls this to run the task immediately. */
void usb_cdc_irq_task(void)
{
    if (!g_hw_ready)
        return;
    usb_task_run(1);
}

/* Called from the 1 ms tick as well as from the console API, so events that
 * arrive while the interrupt is briefly masked still get processed. */
void usb_cdc_poll(void)
{
    uint32_t ints;

    if (!g_hw_ready)
        return;

    /* Fallback for an interrupt that is pending but was never taken.  Seen on
     * hardware: INTS holds BUS_RESET|SETUP_REQ|SUSPEND with the NVIC line
     * enabled and PRIMASK clear, yet no further handler entries.  Servicing
     * the controller from the tick keeps enumeration alive in that state
     * instead of hanging forever; when the interrupt works normally this
     * reads zero and costs one register access per millisecond.
     *
     * BUS_RESET is latched here too: this path used to call dcd_int_handler()
     * directly, bypassing the trampoline, so any reset serviced by polling
     * also skipped the E15 workaround and the device never reconnected. */
    /* dcd_int_handler() is also entered by the USB ISR.  Cross-core USB
     * critical section: the fallback here races the other core's putc()
     * exactly like the ISR path does (see usb_cdc_lock above). */
    {
        uint32_t primask = usb_cdc_lock();

        ints = usb_hw->ints;
        if (ints && USB_LK_OK(primask)) {
            dcd_int_handler(0);
            if (ints & 0x00001000u)      /* USB_INTS_BUS_RESET_BITS */
                usb_cdc_e15_request();
            /* Drop an NVIC pending bit left behind by servicing the
             * controller here; otherwise the trampoline runs again with
             * INTS==0. */
            mmio_write32(ARM_SCS_BASE + 0x280u, 1u << USBCTRL_IRQ);
        }
        usb_cdc_unlock(primask);
    }

    /* E15 must run before tud_task() consumes the queued BUS_RESET event
     * (pico-sdk's ordering), but it waits out 10-15 ms of SE0 and must not
     * do that inside the USB interrupt.  This is thread context during the
     * early enumeration wait and tick context after boot. */
    usb_cdc_e15_service();
    usb_task_run(0);
    {
        uint32_t primask = usb_cdc_lock();

        if (USB_LK_OK(primask))
            usb_txq_drain();
        usb_cdc_unlock(primask);
    }
}

/* ---- console API -------------------------------------------------------- */

void usb_cdc_putc(char c)
{
    /* Nothing is written before the host has configured the port.  A
     * "buffer it anyway" version of this (so a late host still got the
     * banner) made the RP2040 stop mid-boot on the UART as well: every
     * console byte then took the cross-core USB lock and pushed into a
     * FIFO that could not drain until enumeration, and the boot visibly
     * waited for Windows.  Dropping is the contract here - see
     * kernel/plat/console.h. */
    if (!g_hw_ready || !tud_mounted())
        return;

    /* No lock, no TinyUSB, no waiting: append to this core's ring and let
     * the pump move it out.  A full ring drops the byte and counts it; a
     * caller that must not lose output gets a short write() and retries. */
    if (!usb_txq_put((uint32_t)plat_cpu_id() & 1u, (uint8_t)c))
        g_usb_stat[2]++;
}


int usb_cdc_tx_room(void)
{
    uint32_t id;

    if (!g_hw_ready)
        return 0;
    id = (uint32_t)plat_cpu_id() & 1u;
    if (id >= USB_TX_CORES)
        id = 0;
    /* What this core's ring can still take, not TinyUSB's free space:
     * reading the state machine's FIFO without the lock is what this design
     * avoids, and the two differ by at most one pump pass. */
    return (int)((USB_TX_Q - 1u) -
                 ((usb_txq_head[id] + USB_TX_Q - usb_txq_tail[id]) % USB_TX_Q));
}

void usb_cdc_flush(void)
{
    uint32_t tok;

    if (!g_hw_ready)
        return;
    /* Callers include the panic path, which runs masked: take the section
     * only if it is free.  When the other core holds it the bytes are not
     * lost, they go out on the pump's next pass. */
    tok = usb_cdc_trylock();
    if (USB_LK_OK(tok))
        usb_txq_drain();
    usb_cdc_unlock(tok);
}

int usb_cdc_getchar(void)
{
    int c;

    if (!g_hw_ready)
        return -1;

    /* Only the completed-byte ring: this runs with interrupts masked, on
     * core 0, and must never wait for the USB lock or enter TinyUSB. */
    if (usb_rxq_tail == usb_rxq_head)
        return -1;
    c = usb_rxq[usb_rxq_tail];
    usb_rxq_tail = (uint16_t)((usb_rxq_tail + 1u) % USB_RX_Q);
    return c;
}

int usb_cdc_configured(void)
{
    return (g_hw_ready && tud_mounted()) ? 1 : 0;
}

int usb_cdc_hw_ready(void)
{
    return g_hw_ready;
}

/* Drop the D+ pull-up so the host observes a real disconnect.  Called before
 * handing USB back to the ROM for BOOTSEL: a host that never saw SE0 keeps
 * the stale device node and never enumerates the ROM's MSC volume. */
void usb_cdc_disconnect(void)
{
    uint32_t primask;

    if (!g_hw_ready)
        return;
    primask = usb_cdc_lock();
    usb_hw_clear->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS;
    usb_cdc_unlock(primask);
}

int usb_cdc_init_stage(void)
{
    return g_init_stage;
}

uint32_t usb_cdc_pll_boot(void)
{
    return g_pll_cs_boot;
}

/* Force a disconnect/reconnect.  on=0 drops the D+ pull-up so the host tears
 * the device node down; on=1 puts it back.  Only needed as a recovery path
 * when the host has parked the port - tinyusb re-enumerates on its own after
 * a reset, so nothing calls this on the happy path. */
void usb_cdc_replug(int on)
{
    uint32_t primask;

    if (!g_hw_ready)
        return;
    primask = usb_cdc_lock();
    if (USB_LK_OK(primask)) {
        if (on)
            dcd_connect(0);
        else
            dcd_disconnect(0);
    }
    usb_cdc_unlock(primask);
}

/* Bring-up probe: one register per index so the platform's boot trace can
 * print a single line without knowing TinyUSB's types.  These four words are
 * what tell "clocks wrong" apart from "pull-up never went up" apart from
 * "pull-up is up but the host is not answering" (VBUS|J|CONNECTED). */
uint32_t usb_cdc_dbg(uint32_t which)
{
    switch (which) {
    case 0: return usb_hw->sie_ctrl;
    case 1: return usb_hw->sie_status;
    case 2: return usb_hw->main_ctrl;
    case 3: return (tud_inited() ? 2u : 0u) | (tud_mounted() ? 1u : 0u);
    case 4: return g_ss_pads;
    case 5: return g_ss_retrig;
    case 6: return usb_hw->ints;    /* pending, masked interrupt sources    */
    case 7: return usb_hw->buf_status;
    case 8: return mmio_read32(ARM_NVIC_ISER(0));
    /* EP0 buffer as the SIE last saw it: the 18-byte device descriptor plus
     * the two buffer control words.  Comparing these against the descriptor
     * in .rodata separates "wrong bytes on the wire" from "right bytes,
     * wrong timing". */
    case 10: return mmio_read32(USBCTRL_DPRAM_BASE + 0x100u);
    case 11: return mmio_read32(USBCTRL_DPRAM_BASE + 0x104u);
    case 12: return mmio_read32(USBCTRL_DPRAM_BASE + 0x108u);
    case 13: return mmio_read32(USBCTRL_DPRAM_BASE + 0x10cu);
    case 14: return mmio_read32(USBCTRL_DPRAM_BASE + 0x110u);
    case 15: return mmio_read32(USBCTRL_DPRAM_BASE + 0x080u);
    case 9: return (uint32_t)(uintptr_t)g_usb_irq_handler;
    case 16: { extern volatile uint32_t g_usb_irq_count; return g_usb_irq_count; }
    case 17: { uint32_t v; __asm__ volatile("mrs %0, primask" : "=r"(v)); return v; }
    case 18: return mmio_read32(0xE000ED04u);   /* ICSR: VECTACTIVE/PENDING */
    case 19: return usb_hw->inte;               /* USB interrupt enables     */
    case 20: return usb_hw->buf_status;
    case 21: return g_task_calls;               /* tud_task() from poll      */
    case 22: return g_task_isr_calls;           /* tud_task_ext() from IRQ   */
    case 23: return (uint32_t)g_in_task;        /* 1 forever = frozen task   */
    case 24: return mmio_read32(USBCTRL_REGS_BASE + 0x8cu); /* raw INTR     */
    case 25: return g_e15_calls;        /* E15 forced-J runs on BUS_RESET */
    case 26: return (uint32_t)g_e15_fix;
    default: return 0;
    }
}

/* ---- IRQ trace / on-demand replug --------------------------------------- */

void usb_cdc_trace_snap(void)
{
    struct usb_trace_ent *e;
    uint32_t n = g_usb_trace_n++;

    if (!g_hw_ready)
        return;
    e = &g_usb_trace[n & (USB_TRACE_N - 1u)];
    e->ints   = usb_hw->ints;
    /* INTR (0x8c) is the RAW status before the INTE mask.  TinyUSB only
     * enables five sources, so CRC / bit-stuff / RX-timeout errors are
     * invisible in the IRQ trace but show up here - exactly the signature of
     * a marginal SIE/PHY clock. */
    e->raw    = mmio_read32(USBCTRL_REGS_BASE + 0x8cu);
    e->sie    = usb_hw->sie_status;
    e->buf    = usb_hw->buf_status;
    e->setup0 = mmio_read32(USBCTRL_DPRAM_BASE + 0x000u);
    e->setup1 = mmio_read32(USBCTRL_DPRAM_BASE + 0x004u);
    e->ep0in  = mmio_read32(USBCTRL_DPRAM_BASE + 0x080u);
    e->ep0out = mmio_read32(USBCTRL_DPRAM_BASE + 0x084u);
    e->mnt    = usb_cdc_dbg(3);
}

void usb_cdc_trace_dump(void)
{
    uint32_t n = g_usb_trace_n;
    uint32_t start = (n > USB_TRACE_N) ? n - USB_TRACE_N : 0u;

    dbg_uart_puts("[usbtr] n=");
    dbg_uart_hex(n);
    dbg_uart_puts("\n");
    for (uint32_t i = start; i < n; i++) {
        struct usb_trace_ent *e = &g_usb_trace[i & (USB_TRACE_N - 1u)];
        dbg_uart_puts("[tr "); dbg_uart_hex(i);
        dbg_uart_puts("] i=");   dbg_uart_hex(e->ints);
        dbg_uart_puts(" r=");    dbg_uart_hex(e->raw);
        dbg_uart_puts(" s=");    dbg_uart_hex(e->sie);
        dbg_uart_puts(" b=");    dbg_uart_hex(e->buf);
        dbg_uart_puts(" su=");   dbg_uart_hex(e->setup0);
        dbg_uart_puts("/");      dbg_uart_hex(e->setup1);
        dbg_uart_puts(" ep=");   dbg_uart_hex(e->ep0in);
        dbg_uart_puts("/");      dbg_uart_hex(e->ep0out);
        dbg_uart_puts(" m=");    dbg_uart_hex(e->mnt);
        dbg_uart_puts("\n");
    }
    dbg_uart_puts("[usblog] n=");
    dbg_uart_hex(g_usb_log_len);
    dbg_uart_puts("\n");
    {
        unsigned n = g_usb_log_len;
        for (unsigned i = 0; i < n; i++)
            dbg_uart_putc(g_usb_log[i]);
    }
    dbg_uart_puts("\n[/usblog]\n");
}

/* Give the host a fresh, unambiguous SE0 -> J transition while the kernel is
 * fully running (PRIMASK clear, tick alive, USB IRQ taken).  Ctrl-R on UART0
 * triggers this from plat_timer_tick(), which lets one firmware image A/B the
 * "enumerated too early during plat_early_init()" hypothesis. */
void usb_cdc_replug_cycle(void)
{
    if (!g_hw_ready)
        return;
    /* Start both logs at the replug: the boot-time kilobyte is already full
     * of the first (failed) attempt, and Ctrl-T must show the fresh one. */
    g_usb_trace_n = 0;
    g_usb_log_len = 0;
    g_e15_req = 0;
    usb_cdc_disconnect();
    usb_delay_ms(200);
    usb_hw_set->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS;
}


/* ---- descriptors -------------------------------------------------------- */

/* VID/PID: Raspberry Pi vendor id with a TinyOS-specific product id, the same
 * pair the previous hand-written driver used so Windows keeps any COM-port
 * assignment it already made.  bDeviceClass is Misc/Common/IAD rather than
 * "class per interface" because the configuration below starts with an
 * Interface Association Descriptor - a device that uses an IAD has to say so
 * here, and a mismatch there is enough for the host to reject it right after
 * the first GET_DESCRIPTOR. */
static tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = TUSB_CLASS_MISC,
    .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol    = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x2E8A,
    .idProduct          = 0x000A,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void)
{
    return (uint8_t const *)&desc_device;
}

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

/* 9 bytes of configuration descriptor + 66 bytes of CDC-ACM template. */
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

/* Endpoints: 0x81 notification (interrupt IN), 0x02 data OUT, 0x82 data IN.
 * The notification endpoint is mandatory for CDC-ACM - Windows waits for the
 * serial-state notifications before it exposes the port. */
static uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 0, 0x81, 8, 0x02, 0x82, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;                    /* only one configuration */
    return desc_configuration;
}

/* String descriptors are UTF-16LE with a two-byte header, so the buffer is
 * uint16_t and the length/type pair is packed into element 0. */
static uint16_t desc_str[32];

/* The W25Qxx answers command 4Bh with an 8-byte id (see
 * board/rpi-pico/platform.c).  Unlike the RP2350 ports there is no ROM
 * SYS_INFO call here, so the board reads the part itself; a part that does
 * not answer falls back to 16 zeroes rather than a shared "0001". */
static char g_serial[17];

static void serial_from_chip_id(void)
{
    static const char hex[] = "0123456789abcdef";
    uint8_t id[8] = {0};

    if (!plat_flash_unique_id(id)) {
        memcpy(g_serial, "0000000000000000", sizeof(g_serial));
        return;
    }
    for (int i = 0; i < 8; i++) {
        g_serial[2 * i]     = hex[id[i] >> 4];
        g_serial[2 * i + 1] = hex[id[i] & 0x0fu];
    }
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static char const *const str_table[] = {
        "TinyOS",                   /* iManufacturer */
        "TinyOS CDC-ACM",           /* iProduct      */
        g_serial,                   /* iSerialNumber: the flash's own id */
    };
    uint8_t chr_count;

    (void)langid;

    if (index == 0) {
        desc_str[1] = 0x0409;       /* English (United States) */
        chr_count = 1;
    } else {
        if (index > sizeof(str_table) / sizeof(str_table[0]))
            return 0;
        const char *s = str_table[index - 1];
        chr_count = (uint8_t)strlen(s);
        if (chr_count > 31)
            chr_count = 31;         /* desc_str has room for 31 characters */
        for (uint8_t i = 0; i < chr_count; i++)
            desc_str[1 + i] = (uint16_t)(uint8_t)s[i];
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2u * chr_count + 2u));
    return desc_str;
}

/* ---- TinyUSB log sink ---------------------------------------------------- */

/* CFG_TUSB_DEBUG_PRINTF (tusb_config.h) points here.  TinyUSB calls it from
 * both the USB interrupt and thread context, so format onto the caller's
 * stack (no shared state) and append under a short PRIMASK window.  The log
 * is dumped later by usb_cdc_trace_dump(); it never goes through the console
 * or the CDC FIFO, so it cannot recurse or depend on enumeration. */
int usb_dbg_printf(const char *fmt, ...)
{
    char buf[160];
    __builtin_va_list ap;
    uint32_t primask;
    unsigned room, i;
    int n;

    if (g_usb_log_len >= USB_LOG_SIZE)
        return 0;                       /* full: do not even format */
    __builtin_va_start(ap, fmt);
    n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    if (n <= 0)
        return n;
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;

    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");
    room = USB_LOG_SIZE - g_usb_log_len;
    if (room > (unsigned)n)
        room = (unsigned)n;
    for (i = 0; i < room; i++)
        g_usb_log[g_usb_log_len + i] = buf[i];
    g_usb_log_len += room;
    __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
    return n;
}
