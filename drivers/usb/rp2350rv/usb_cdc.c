/* USB CDC-ACM console for the RP2350, built on TinyUSB.
 *
 * History worth keeping: this started as a hand-written register-level
 * driver, and getting enumeration to work meant finding eight separate bugs
 * (pad isolation, GPIO46/47 not muxed to the PHY, EP0 buffer address,
 * EP0_INT_1BUF, the missing buffer-control SEL/LAST bits, descriptor
 * contents, the pending-address ordering, and SET_ADDRESS timing).  The
 * TinyUSB device controller driver is the reference implementation for this
 * exact SIE and already gets all of those right, so the protocol layer is
 * now delegated to it and this file only owns what is board specific:
 *
 *   - PLL_USB -> 48 MHz on clk_usb (RP2350 has a separate CLK_USB_DIV
 *     register and a different PLL base than the RP2040)
 *   - RESETS_USBCTRL is bit 28 here, and its reset-done bit does not come
 *     back until clk_usb is actually running
 *   - GPIO46/47 must be muxed to function 31 and their pad ISO bit cleared,
 *     otherwise the PHY drives the pins but the SIE never sees a packet
 *
 * Threading model: USBCTRL (IRQ 14) runs TinyUSB's dcd_int_handler, which
 * does nothing but timestamp events into a queue.  All the actual work
 * happens in tud_task(), called from usb_cdc_poll() on every 1 ms tick and
 * from the console API - the same split pico-sdk uses for stdio_usb, which
 * drives tud_task from a 1 ms repeating timer.
 *
 * Configuration lives in board/rp2350/tusb_config.h; the pico-sdk facade the
 * upstream port needs is in third_party/tinyusb/port_rp2 and tusb_shim.c.
 */

/* Take the USB controller's interrupt on Hazard3 instead of polling it.
 *
 * Hazard3 has no NVIC: external lines arrive as mcause 11 (machine external)
 * and the per-line enable/pending bits live in the XH3IRQ CSR arrays
 * meiea/meipa (0xbe0/0xbe1; element 0 covers lines 0..31).  USBCTRL_IRQ is
 * line 14 on the RP2350.  Off by default because the polled path is the one
 * that has run on hardware; flip to 1 (or -DTOS_RV_USB_IRQ=1) to A/B it.
 * The idle poll stays in place either way, so a missed interrupt degrades to
 * the old behaviour instead of a dead console. */
#ifndef TOS_RV_USB_IRQ
#define TOS_RV_USB_IRQ 1
#endif

#include "platform.h"
#include "board.h"
#include "rp2350rv_memmap.h"
#include "usb_cdc.h"

#include "tusb.h"
#include "device/usbd_pvt.h"
#include "device/dcd.h"
#include "hardware/structs/usb.h"
#include "string.h"
#include "console.h"
#include "cpu.h"

/* SIO hardware spinlock 30 (0xD0000000 + 0x100 + 4*30).  31 is reserved for
 * the kernel's ARM spin gate; on the RV port the kernel uses LR/SC, so 30 is
 * free either way.  The SIO block is shared between the two harts whatever
 * the ISA, so this is the natural cross-core mutex.  Defined at the top
 * because usb_cdc_init() force-releases it before anything else can take
 * it - the SIO block survives a warm reboot and a previous run may have
 * died holding the lock. */
#define USB_SIO_SPINLOCK (*(volatile uint32_t *)(0xD0000000u + 0x100u + 4u * 30u))

/* Cross-core USB critical section.  TinyUSB state was guarded only by
 * per-hart interrupt masking (plat_irq_save), which stops re-entrancy on
 * ONE hart; the other hart's producer (usb_cdc_putc feeds the ring, its
 * tick drains it) raced this hart's tud_task()/write_flush across cores
 * and could corrupt tu_fifo bookkeeping - the same failure that wedged
 * rp2350smp's idle core forever inside tty_poll.  Hold times are bounded:
 * tud_task() drains the event queue and returns; nothing inside waits on
 * the other hart. */
static uint32_t usb_critical_enter(void)
{
    uint32_t irq = plat_irq_save();

    /* Non-zero = acquired.  RP2350 answers one-hot (bit 30 for lock 30); a
     * bit-31 test reads that as "still held" and hangs the console path. */
    while (USB_SIO_SPINLOCK == 0u)
        ;
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    return irq;
}

static void usb_critical_exit(uint32_t irq)
{
    __asm__ volatile("fence iorw, iorw" ::: "memory");
    USB_SIO_SPINLOCK = 0;
    plat_irq_restore(irq);
}

/* TinyUSB declares this weak and only calls it when the symbol is non-NULL.
 * An *undefined* weak symbol is not 0 everywhere: on RV32 with the medany
 * code model the reference is PC-relative, lld hands back a non-zero address,
 * the NULL test passes and the call jumps into nothing - which kept the
 * RP2350 RISC-V image from booting unless the ROM had already brought USB up.
 * A definition that reports "no application driver" gives the same answer on
 * every target, and stays weak so an application can still replace it. */
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

#define RESETS_USBCTRL    0x10000000u   /* RP2040 had this at bit 24        */
#define RESETS_PLL_USB    0x00008000u

#define PLL_USB_BASE      0x40058000u   /* RP2040: 0x4002c000               */
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

/* RP2350 splits the slice: CLK_USB_CTRL at 0x60 holds ENABLE + AUXSRC,
 * CLK_USB_DIV at 0x64 holds INT at bits 19:16.  AUXSRC = 0 is clksrc_pll_usb. */
#define CLOCKS_CLK_USB_CTRL (*(volatile uint32_t *)(0x40010000u + 0x60u))
#define CLOCKS_CLK_USB_DIV  (*(volatile uint32_t *)(0x40010000u + 0x64u))
#define CLK_USB_ENABLE      0x00000800u
#define CLK_USB_DIV_INT_1   (1u << 16)
#define CLK_USB_AUX_PLL_USB 0x00000000u

/* ---- state -------------------------------------------------------------- */

static int g_hw_ready;          /* clocks + controller up                 */
static int g_init_stage;        /* 0 ok, 1 reset timeout, 2 PLL timeout   */
static uint32_t g_pll_cs_boot;  /* PLL_CS as the ROM handed it over       */
static uint32_t g_ss_pads;      /* SIE_STATUS right after the pad writes  */
static uint32_t g_ss_retrig;    /* ... and after the pull-up retrigger    */

/* ---- cross-core transmit ring ------------------------------------------ */

/* TinyUSB is not reentrant and its task loop runs on CPU 0 only, but either
 * hart can print (the shell migrates).  Producers append to this ring under
 * a short spin lock; CPU 0 drains it inside usb_cdc_poll(), where it also
 * owns the controller state machine.  1 KiB covers a burst like `help`
 * while CPU 0 is between polls; when it fills, bytes are dropped, which is
 * the same policy the 128-byte CDC FIFO already has. */
#define USB_TX_RING 1024u

static volatile int      g_tx_lock;
static volatile uint8_t  g_tx_ring[USB_TX_RING];
static volatile uint32_t g_tx_head;
static volatile uint32_t g_tx_tail;

/* ---- no-host-reader backlog guard ---------------------------------------
 *
 * A configured CDC device whose bulk IN transfer is never completed means
 * nobody on the host is draining the endpoint.  TinyUSB copes with that on
 * its own: SET_CONTROL_LINE_STATE clears DTR and makes tx_ff overwritable
 * (cdc_device.c:418), i.e. old bytes are discarded when the FIFO fills.
 * Our 1024-byte ring defeated that: usb_tx_drain() used to stop as soon as
 * write_available() hit zero, so a full FIFO parked *all* newer output in
 * the ring, and every 1 ms poll re-attempted the same drain + flush against
 * a permanently pending xfer.  On this port that work is CPU0-only and has
 * no USB IRQ to share it, which is what made the two harts asymmetric.
 *
 * Rule: there is a reader if DTR is asserted or a bulk IN completed recently
 * (the latter covers usbser.sys reading with DTR held low).  With no reader,
 * output is dropped at the producer, except for one probe byte every
 * CDC_PROBE_MS.  The probe is what lets a terminal that appears later bring
 * the path back - without it the FIFO would stay empty forever and no
 * completion could ever clear the state.  No ring is accumulated, so the
 * endpoint never ends up with a permanently pending full xfer.
 *
 * Everything here lives on the producer side (usb_cdc_putc).  The 1 ms
 * usb_cdc_poll()/usb_tx_drain() path is left alone: adding even a handful
 * of instructions there changed smpbench by 0.15x in both directions
 * during the A/B runs, which would fake any performance result.
 *
 * g_cdc_fix (kernel/procfs.c, flipped by /proc/cdcfix) is a run-time A/B
 * switch so fix=0 and fix=1 can be compared within one firmware image. */
#define CDC_AGE_MS      200u
#define CDC_PROBE_MS    100u

extern volatile uint32_t g_cdc_fix;
extern volatile uint32_t g_cdc_diag[6];

static volatile uint32_t g_tx_done_ms;    /* last bulk IN completion       */
static volatile uint32_t g_putc_probe_ms; /* last probe byte admitted      */

static uint32_t cdc_idle_ms(void)
{
    return (uint32_t)plat_uptime_ms() - g_tx_done_ms;
}

static int cdc_has_reader(void)
{
    return tud_cdc_connected() || (cdc_idle_ms() < CDC_AGE_MS);
}

/* TinyUSB weak callback: a completed IN transfer is the only reliable proof
 * that the host is taking data (it works even when DTR is held low). */
void tud_cdc_tx_complete_cb(uint8_t itf)
{
    (void)itf;
    g_tx_done_ms = (uint32_t)plat_uptime_ms();
}

/* Real definitions for TinyUSB callbacks this port never overrides.
 *
 * On Cortex-M an undefined weak symbol links to 0 and the stack's own
 * `if (cb)` guards skip the call.  Hazard3 has no relocations the ARM tool
 * chain shares: under RV32 medany GNU ld resolves an undefined weak call to
 * a thunk that jumps *to itself* (auipc x1,0; jalr x1,0(x1) with x1 set to
 * the thunk), so the null check reads a non-zero garbage address and the
 * call hangs CPU 0 forever - ticks keep firing and echoing keystrokes, but
 * no process is ever scheduled again.  This was observed live: a physical
 * RESET with VBUS held up makes Windows replay SET_CONTROL_LINE_STATE while
 * re-enumerating, whose ACK stage calls tud_cdc_line_state_cb, and the
 * shell then stopped responding to every keystroke.  Same root cause as
 * usbd_app_driver_get_cb before it; every callback the stack can reach must
 * therefore have a real definition. */
void tud_cdc_rx_cb(uint8_t itf)
{
    (void)itf;
}

void tud_cdc_rx_wanted_cb(uint8_t itf, char wanted_char)
{
    (void)itf;
    (void)wanted_char;
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *p_line_coding)
{
    (void)itf;
    (void)p_line_coding;
}

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf;
    (void)dtr;
    (void)rts;
}

void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms)
{
    (void)itf;
    (void)duration_ms;
}

/* dcd.h declares this weak and usbd.c calls it for ISO endpoints.  This
 * port has no ISO endpoints and no implementation; return failure so the
 * caller just marks the endpoint idle instead of jumping into a thunk. */
bool dcd_edpt_xfer_fifo(uint8_t rhport, uint8_t ep_addr, tu_fifo_t *ff,
                        uint16_t total_bytes)
{
    (void)rhport;
    (void)ep_addr;
    (void)ff;
    (void)total_bytes;
    return false;
}

/* ---- GPIO46/47 -> USB PHY ----------------------------------------------- */

/* Two things have to happen, and only the muxing one is obvious:
 *
 *   1. IO_BANK0 CTRL for GPIO46/47 must select function 31 (USB PHY).  A
 *      freshly reset pad is function 0 (SPI/XIP), so the controller is not
 *      wired to the pins at all.  This is invisible from the USB side: the
 *      PHY still reports a valid J state and SIE_STATUS.CONNECTED still
 *      reads 1, because those come from the analogue side, while nothing
 *      reaches the host.
 *
 *   2. The pad itself must have its input buffer enabled.  PADS_BANK0 for
 *      GPIO46/47 reads back as 0 on this chip, so a read-modify-write (the
 *      obvious way to clear the ISO bit) does nothing: the value has to be
 *      written outright.  With IE clear the digital input never reaches the
 *      SIE, and the symptom is a bus that looks permanently SE0 -
 *      SIE_STATUS reads VBUS only, CONNECTED stays 0, and the host never
 *      sees a device even though the pull-up is enabled.  Writing
 *      IE|SCHMITT|PDE (0x46) fixes it and the status jumps straight to
 *      VBUS|J|CONNECTED (0x00010005).
 *
 * Both were verified on real hardware by writing the register from the boot
 * path and watching SIE_STATUS change in the same run. */
static void usb_pads_setup(void)
{
    mmio_write32(IO_BANK0_BASE + 0x04u + 46u * 8u, 31u);
    mmio_write32(IO_BANK0_BASE + 0x04u + 47u * 8u, 31u);

    /* PADS_BANK0: one 4-byte register per GPIO from 0x04, so GPIO46/47 are
     * 0xbc/0xc0 - not 0x84/0x88, which belong to GPIO32/33.
     *
     * 0x156 is the documented reset value (0x116 = ISO|PDE|SCHMITT|
     * DRIVE=4mA) plus IE (bit 6).  That is exactly what pico-sdk leaves
     * behind after gpio_set_function(): it only sets IE and clears OD, so
     * ISO stays asserted and the drive strength stays at 4 mA.  Writing a
     * hand-built 0x46 instead (ISO clear, DRIVE=2mA) still produced a
     * connected bus, but enumeration failed after the first descriptor -
     * the same place the hand-written driver had always died - so the pad
     * value is now the SDK's rather than a guess. */
    mmio_write32(PADS_BANK0_BASE + 0xbcu, 0x156u);
    mmio_write32(PADS_BANK0_BASE + 0xc0u, 0x156u);
}

/* Bring-up breadcrumbs.  The USB controller hangs the bus the moment a
 * register is touched while clk_usb is stopped, so a failure here leaves no
 * output at all; these markers are the only way to see how far it got. */
static void u_mark(const char *s)
{
    while (*s)
        plat_console_putc(*s++);
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
     * yet: on the RP2350 the USBCTRL done bit stays low until clk_usb is
     * actually running, so waiting here times out on real hardware (it reads
     * done=0x06c286c0 with bit 28 clear). */
    m = mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET);
    m &= ~(RESETS_USBCTRL | RESETS_PLL_USB);
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET, m);

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
        for (spins = 0; spins < 1000000u; spins++) {
            if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & RESETS_PLL_USB)
                break;
        }

        PLL_CS_REG = PLL_REFDIV;        /* 12 MHz * 100 = 1200 MHz VCO      */
        PLL_FBDIV_REG = PLL_FBDIV;

        /* Only PD and VCOPD come off here; DSMPD and POSTDIVPD stay set. */
        PLL_PWR_REG &= ~(PLL_PWR_PD | PLL_PWR_VCOPD);
        for (spins = 0; spins < 1000000u; spins++) {
            if (PLL_CS_REG & PLL_CS_LOCK)
                break;
        }
        if (spins == 1000000u) {
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
    for (spins = 0; spins < 1000000u; spins++) {
        if ((mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) &
             (RESETS_USBCTRL | RESETS_PLL_USB)) ==
            (RESETS_USBCTRL | RESETS_PLL_USB))
            break;
    }
    if (spins == 1000000u) {
        g_init_stage = 1;
        return -1;                      /* no USB: UART-only console        */
    }
    return 0;
}

/* ---- bring-up ----------------------------------------------------------- */

void usb_cdc_init(void)
{
    g_hw_ready = 0;
    g_init_stage = 0;
    u_mark("[u0]\r\n");

    /* A warm boot can land with the cross-core USB spinlock still held by
     * the previous run - the SIO block survives a hart reset.  This is the
     * first touch, before either hart can take the lock again. */
    USB_SIO_SPINLOCK = 0;

    /* Order matters: PLL and clk_usb first (the controller's reset-done bit
     * depends on the clock), then the pads, then TinyUSB - whose dcd_init()
     * resets the controller itself and would undo any register-level setup
     * done before it. */
    if (usb_clock_init() != 0)
        return;
    u_mark("[u1] clk\r\n");

    usb_pads_setup();
    u_mark("[u2] pads\r\n");

    /* Mark ready before TinyUSB can raise an interrupt: the handler bails
     * out on !g_hw_ready, and an interrupt that is never drained keeps
     * re-asserting, which is how an earlier build spun forever. */
    g_hw_ready = 1;

    if (!tud_init(0)) {
        g_hw_ready = 0;
        return;
    }
    u_mark("[u3] tud\r\n");

    /* And again, now that TinyUSB's dcd_init() has reset the controller and
     * cleared MAIN_CTRL.PHY_ISO.  The same writes issued *before* that reset
     * do not take effect: measured on hardware, pads written ahead of
     * tud_init() leave SIE_STATUS at VBUS-only (SE0), while writing them
     * after it turns the bus into VBUS|J|CONNECTED.  The controller reset is
     * the only step in between, so the pad write has to follow it. */
    usb_pads_setup();
    g_ss_pads = usb_hw->sie_status;

    /* The pull-up was raised inside tud_init(), before the pad's input buffer
     * existed.  Drop and re-raise it so the connect is detected against the
     * configured pad instead of an isolated one - the same sequence that
     * turned SIE_STATUS from 0x00000001 into 0x00010005 when it was tried by
     * hand after boot. */
    usb_hw_clear->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS;
    for (volatile int i = 0; i < 20000; i++)
        ;
    usb_hw_set->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS;
    for (volatile int i = 0; i < 20000; i++)
        ;
    g_ss_retrig = usb_hw->sie_status;
    u_mark("[u4] pullup\r\n");
}

/* Open the interrupt gate.  The RISC-V port polls the controller instead:
 * Hazard3 delivers external lines through its own CSR window, and
 * usb_cdc_poll() (from the console read path and the tight enumeration
 * wait) is what drains usb_hw->ints.  Kept as a function so the bring-up
 * sequence reads the same as the ARM board. */
void usb_cdc_irq_enable(void)
{
#if TOS_RV_USB_IRQ
    /* meiea: enable line 14; mie.MEIE: let the core take cause 11 at all.
     * mstatus.MIE is opened later by the scheduler. */
    __asm__ volatile("csrs 0xbe0, %0" :: "r"(1u << 14u));
    __asm__ volatile("csrs mie, %0"   :: "r"(1u << 11u));
#endif
}

/* Wait (bounded) for the host to finish SET_CONFIGURATION so the boot banner
 * is not written into a FIFO nobody reads yet.  The ROM may hand over clk_sys
 * anywhere between 12 and 150 MHz, so the bound is derived from the measured
 * clock: one spin is about 130 cycles and 1.5 s is plenty for a host that is
 * awake, without stalling a board that has no USB cable attached. */
static void usb_cdc_wait(uint32_t clk_hz)
{
    uint64_t spins;

    if (!g_hw_ready)
        return;
    spins = ((uint64_t)clk_hz * 3ull) / (2ull * 130ull);
    for (uint64_t i = 0; i < spins && !tud_mounted(); i++) {
        usb_cdc_poll();
        for (volatile int j = 0; j < 40; j++)
            ;
    }
}

void usb_cdc_console_init(uint32_t clk_hz)
{
    usb_cdc_init();
    usb_cdc_wait(clk_hz);
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
static volatile int g_in_task;

static void usb_task_run(int from_isr)
{
    uint32_t irq = usb_critical_enter();

    if (g_in_task) {
        usb_critical_exit(irq);
        return;                     /* the outer call will drain the queue */
    }
    g_in_task = 1;

    if (from_isr)
        tud_task_ext(0, true);
    else
        tud_task();

    g_in_task = 0;
    usb_critical_exit(irq);
}

/* IRQ 14, called from the shared vector table through g_usb_irq_handler.
 * dcd_int_handler is TinyUSB's own; it has already been registered by
 * dcd_init() and is invoked by the trampoline in tusb_shim.c, which then
 * calls this to run the task immediately. */
void usb_cdc_irq_task(void)
{
    if (!g_hw_ready)
        return;

    /* Service the controller itself in the interrupt, not only its event
     * queue: dcd_rp2040_irq() clears INTR/SIE_STATUS and queues the event.
     * Running only the task here would leave SETUP_REQ|BUFF_STATUS asserted
     * (a physical RESET with VBUS up keeps the host re-enumerating), the
     * external line latched, and the trap re-entering forever.  This mirrors
     * exactly what the ARM port's IRQ trampoline does before its own task
     * run. */
    if (usb_hw->ints) {
        uint32_t irq = usb_critical_enter();

        dcd_int_handler(0);
        usb_critical_exit(irq);
    }

    usb_task_run(1);
}

/* Called from the 1 ms tick as well as from the console API, so events that
 * arrive while the interrupt is briefly masked still get processed.  The
 * whole body is single-entry: the tick can interrupt a thread-context poll.
 * Must run on CPU 0 - it is the only owner of the controller state machine. */
static volatile int g_in_poll;

static void usb_tx_drain(void)
{
    uint32_t irq = plat_irq_save();
    uint32_t usbpm;

    plat_spin_acquire(&g_tx_lock);
    /* The ring bookkeeping is g_tx_lock's business; the CDC FIFO the loop
     * feeds is TinyUSB state and races the other hart's tud_task() - so the
     * fifo access sits inside the cross-core USB critical section.  Order
     * is always g_tx_lock then the USB lock, and no g_tx_lock holder ever
     * waits for the USB lock except this drain, so the two cannot cycle. */
    usbpm = usb_critical_enter();
    while (g_tx_tail != g_tx_head && tud_cdc_write_available() != 0) {
        tud_cdc_write_char(g_tx_ring[g_tx_tail % USB_TX_RING]);
        g_tx_tail++;
    }
    usb_critical_exit(usbpm);
    plat_spin_release(&g_tx_lock);
    plat_irq_restore(irq);
}

/* Move whatever the ring holds into the CDC FIFO.  Deliberately does not run
 * tud_task(): this is called from the 1 ms tick, and the state machine must
 * stay in thread context (see usb_cdc_poll).  Without this a program that
 * never blocks cannot print more than the 1 KiB ring. */
void usb_cdc_tx_kick(void)
{
    if (g_hw_ready)
        usb_tx_drain();
}

void usb_cdc_poll(void)
{
    uint32_t irq;

    if (!g_hw_ready)
        return;

    irq = plat_irq_save();
    if (g_in_poll) {
        plat_irq_restore(irq);
        return;                     /* the outer call will drain the queue */
    }
    g_in_poll = 1;
    plat_irq_restore(irq);

    /* Deliberately untouched by the no-reader fix: measurements showed that
     * adding even a handful of instructions or volatile diagnostic writes to
     * this CPU0-only 1 ms path changes smpbench by 0.15x.  Dropping stale
     * output happens entirely on the producer side instead. */

    /* Polled controller: any pending SIE event is serviced here instead of
     * in an IRQ 14 handler (Hazard3 external lines need its CSR window). */
    if (usb_hw->ints) {
        uint32_t usbpm = usb_critical_enter();

        dcd_int_handler(0);
        usb_critical_exit(usbpm);
    }

    usb_task_run(0);
    usb_tx_drain();
    {
        uint32_t usbpm = usb_critical_enter();

        if (tud_cdc_write_available() < CFG_TUD_CDC_TX_BUFSIZE)
            tud_cdc_write_flush();
        usb_critical_exit(usbpm);
    }

    g_in_poll = 0;
}

/* ---- console API -------------------------------------------------------- */

void usb_cdc_putc(char c)
{
    uint32_t irq;
    uint32_t next;
    int drop = 0;

    if (!g_hw_ready || !tud_mounted())
        return;

    /* Refreshed on every output character so /proc/cdcstat always sees a
     * recent ring snapshot.  This is the producer side, never the 1 ms tick. */
    g_cdc_diag[1] = g_tx_head - g_tx_tail;

    if (g_cdc_fix && !cdc_has_reader()) {
        uint32_t now = (uint32_t)plat_uptime_ms();

        g_cdc_diag[0] = 1u;               /* no_reader                     */
        g_cdc_diag[4] = cdc_idle_ms();     /* ms since last IN completion   */
        g_cdc_diag[5] = tud_cdc_get_line_state();

        /* Drop console output while no host is reading; the UART mirror has
         * already been written by plat_console_putc().  Admit one byte per
         * probe interval so a terminal that appears can complete an IN
         * transfer and re-arm the path. */
        if (now - g_putc_probe_ms < CDC_PROBE_MS)
            drop = 1;
        else {
            g_putc_probe_ms = now;
            g_cdc_diag[3]++;               /* probe bytes admitted          */
        }
    } else {
        g_cdc_diag[0] = 0u;
    }

    /* Enqueue only: the producer may be CPU 1, while TinyUSB's FIFO and
     * task loop belong to CPU 0.  The spin lock is held for a handful of
     * instructions, and interrupts are off locally so the tick cannot
     * re-enter this on the same core. */
    irq = plat_irq_save();
    plat_spin_acquire(&g_tx_lock);

    if (drop) {
        /* No reader: discard this byte AND any backlog left over from the
         * reader that just went away.  Without this, 200+ bytes survive in
         * the ring behind a FIFO that never drains.  TinyUSB's own tx_ff is
         * left alone - it is overwritable when DTR is clear, and touching it
         * from an arbitrary hart/context is not safe. */
        g_cdc_diag[2] += (g_tx_head - g_tx_tail) + 1u;
        g_tx_tail = g_tx_head;
        plat_spin_release(&g_tx_lock);
        plat_irq_restore(irq);
        return;
    }

    next = g_tx_head + 1;
    if (next - g_tx_tail <= USB_TX_RING) {
        g_tx_ring[g_tx_head % USB_TX_RING] = (uint8_t)c;
        g_tx_head = next;
    }
    plat_spin_release(&g_tx_lock);
    plat_irq_restore(irq);
}

int usb_cdc_tx_room(void)
{
    if (!g_hw_ready)
        return 0;
    return (int)tud_cdc_write_available();
}

void usb_cdc_flush(void)
{
    uint32_t usbpm;

    if (!g_hw_ready)
        return;
    /* Only CPU 0 owns the controller; a flush on the other hart just leaves
     * the bytes for the next poll. */
    if (plat_cpu_id() != 0)
        return;
    usbpm = usb_critical_enter();
    usb_tx_drain();
    tud_cdc_write_flush();
    usb_critical_exit(usbpm);
}

int usb_cdc_getchar(void)
{
    uint32_t usbpm;
    int32_t c;

    if (!g_hw_ready)
        return -1;
    usbpm = usb_critical_enter();
    c = tud_cdc_read_char();
    usb_critical_exit(usbpm);
    return (c < 0) ? -1 : (int)c;
}

int usb_cdc_configured(void)
{
    return (g_hw_ready && tud_mounted()) ? 1 : 0;
}

int usb_cdc_hw_ready(void)
{
    return g_hw_ready;
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
    if (!g_hw_ready)
        return;
    if (on)
        dcd_connect(0);
    else
        dcd_disconnect(0);
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
    /* EP0 buffer as the SIE last saw it: the 18-byte device descriptor plus
     * the two buffer control words. */
    case 10: return mmio_read32(USBCTRL_DPRAM_BASE + 0x100u);
    case 11: return mmio_read32(USBCTRL_DPRAM_BASE + 0x104u);
    case 12: return mmio_read32(USBCTRL_DPRAM_BASE + 0x108u);
    case 13: return mmio_read32(USBCTRL_DPRAM_BASE + 0x10cu);
    case 14: return mmio_read32(USBCTRL_DPRAM_BASE + 0x110u);
    case 15: return mmio_read32(USBCTRL_DPRAM_BASE + 0x080u);
    default: return 0;
    }
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

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static char const *const str_table[] = {
        "TinyOS",                   /* iManufacturer */
        "TinyOS CDC-ACM",           /* iProduct      */
        "0001",                     /* iSerialNumber */
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

/* CFG_TUSB_DEBUG_PRINTF (tusb_config.h) points here so TinyUSB's own log
 * comes out on UART0.  Formatting goes through the kernel's kvsnprintf rather
 * than a private implementation, and the output bypasses the CDC path on
 * purpose: a log message must not depend on the device having enumerated. */
int usb_dbg_printf(const char *fmt, ...)
{
    char buf[160];
    __builtin_va_list ap;
    int n;

    __builtin_va_start(ap, fmt);
    n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    if (n <= 0)
        return n;
    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    for (int i = 0; i < n; i++)
        plat_console_putc(buf[i]);
    return n;
}
