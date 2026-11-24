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

#include "platform.h"
#include "board.h"
#include "arm_memmap.h"
#include "scs.h"
#include "usb_cdc.h"

#include "tusb.h"
#include "device/usbd_pvt.h"


#include "device/dcd.h"
#include "hardware/structs/usb.h"

/* SIO hardware spinlocks (0xD0000000 + 0x100 + 4*n).  31 is the kernel's spin
 * gate, so the USB section picks from below that.  Defined at the top because
 * usb_cdc_init() force-releases the candidates before probing them. */
#define SIO_LK(n) ((volatile uint32_t *)(0xD0000000u + 0x100u + 4u * (n)))

/* Return token bits.  PRIMASK is a single bit, so the upper bits are free to
 * carry state (USB_LK_FAILED lives in usb_cdc.h because the shim tests it):
 *   bit31 - this call only bumped a depth counter, the outer section locks
 *   bit30 - no spinlock was offered at boot: run without one (reported)
 *   bit29 - the bounded acquire gave up: the caller must skip the work */
#define USB_LK_NESTED 0x80000000u
#define USB_LK_NONE   0x40000000u
/* ~20 ms at 150 MHz: long enough that a normal cross-core hold never trips
 * it, short enough that a real deadlock does not look like a dead board. */
#define USB_LK_SPINS  300000u

static void usb_lk_select(void);

#include "string.h"
#include "console.h"
#include "diag.h"

/* Fills g_serial from the chip id; defined with the descriptors below. */
static void serial_from_chip_id(void);

#if TOS_DIAG
/* Bring-up markers straight to the PL011 (board/rp2350/platform.c).  They
 * must not go through the console: this file can be called from inside a USB
 * critical section, and a re-entrant console write there is exactly the kind
 * of hang these markers exist to find. */
extern void plat_boot_mark(const char *s);
extern void plat_boot_mark_u32(uint32_t v);
extern void plat_boot_mark_hex(uint32_t v);
#define BM(s) plat_boot_mark(s)
/* Names the USB step this core is inside, so a core that wedges with
 * interrupts masked can be located from the other core. */
#define US(code) (g_usb_site[plat_cpu_id() & 1] = (code))
#else
#define BM(s) do { } while (0)
#define US(code) do { } while (0)
#endif

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

    /* /proc/usbboot: what the boot path left behind.  USBCTRL is released
     * only by a boot path that used it - the BOOTSEL mass-storage ROM - so
     * this one bit separates "the host is still holding the device node from
     * before" from "the cable was never enumerated".  It is the candidate
     * signal for skipping the 200 ms D+ drop on cold boots (OPEN-ITEMS 3). */
    g_usb_boot[0] = mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET);
    g_usb_boot[1] = (g_usb_boot[0] & RESETS_USBCTRL) ? 0u : 1u;
    serial_from_chip_id();

    /* A warm boot (panic path, soft reboot) can land with a cross-core USB
     * spinlock still held by the previous run - the SIO block survives a core
     * reset.  Any write releases, so clear the candidates before probing
     * which one this board actually grants. */
    *SIO_LK(30) = 0;
    *SIO_LK(29) = 0;
    *SIO_LK(28) = 0;
    usb_lk_select();

    /* Order matters: PLL and clk_usb first (the controller's reset-done bit
     * depends on the clock), then the pads, then TinyUSB - whose dcd_init()
     * resets the controller itself and would undo any register-level setup
     * done before it. */
    if (usb_clock_init() != 0) {
        BM("[u2x]");
        return;
    }

    usb_pads_setup();

    /* Mark ready before TinyUSB can raise an interrupt: the handler bails
     * out on !g_hw_ready, and an interrupt that is never drained keeps
     * re-asserting, which is how an earlier build spun forever. */
    g_hw_ready = 1;

    if (!tud_init(0)) {
        g_hw_ready = 0;
        BM("[u4x]");
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
    for (volatile int i = 0; i < 20000; i++)
        ;
    g_usb_boot[6] = 1u;                     /* the D+ drop ran ...       */
    g_usb_boot[7] = usb_hw->sie_status;     /* ... and the bus went idle */
    usb_hw_set->sie_ctrl = USB_SIE_CTRL_PULLUP_EN_BITS;
    for (volatile int i = 0; i < 20000; i++)
        ;
    g_ss_retrig = usb_hw->sie_status;
    g_usb_boot[4] = g_ss_pads;
    g_usb_boot[5] = g_ss_retrig;
}

/* Open the NVIC gate for IRQ 14.  Kept separate from init so the ordering is
 * explicit: controller configured and pull-up already up, interrupt delivery
 * second. */
void usb_cdc_irq_enable(void)
{
    if (!g_hw_ready)
        return;
    dcd_int_enable(0);
}

/* The console does not wait for the host.  The old bounded spin (nominally
 * 1.5 s, 8-10 s in practice because every iteration also ran tud_task()) kept
 * the whole boot on the host's enumeration: measured on the RP2040 as ten
 * seconds of solid LED after every reset.  See drivers/usb/rp2040/usb_cdc.c
 * for the timestamps. */

/* Pump the controller for a short, bounded time right after init, so the
 * host's first control transfers are answered during boot instead of timing
 * out and backing off (Windows retries after ~5.5 s).
 *
 * Without this the host's first SETUP arrives while nothing is running the
 * USB task (early boot still has interrupts masked), so Windows times out and
 * retries: measured on ATK-DNRP2350AM M33, power cycles alternated between
 * 5.5 s and 11.3 s "device gone -> CDC back" even though the firmware itself
 * was up in well under a second.
 *
 * The budget is a cycle window, not a fixed iteration count: at 150 MHz,
 * 40 000 polls measure ~250 ms, so the count is derived from the running
 * clock (this also covers the 12 MHz crystal-failure path, where a fixed
 * count would take 12x as long in wall time).  It stops as soon as the port
 * is configured. */
static void usb_cdc_boot_pump(void)
{
    uint32_t n = plat_cpu_hz() / 3750u;      /* 150 MHz -> 40 000, ~250 ms */

    if (n > 200000u)
        n = 200000u;                         /* never more than ~1.3 s */
    if (n < 4000u)
        n = 4000u;                           /* and always a few ms' worth */
    for (uint32_t i = 0; i < n && !tud_mounted(); i++)
        usb_cdc_poll();
}

void usb_cdc_console_init(uint32_t clk_hz)
{
    (void)clk_hz;
    usb_cdc_init();
    usb_cdc_boot_pump();
}

/* ---- cross-core USB exclusion ------------------------------------------- */

static volatile uint32_t *usb_lk_reg;      /* chosen hardware spinlock, 0 = none */
static uint32_t           usb_lk_bit;      /* its bit in the status word */
static volatile uint32_t  usb_depth[2];
volatile uint32_t         g_usb_lock_fail;

/* Live counters for /proc/usbboot: [0] lock timeouts, [1] "booted with no
 * spinlock" (1 = yes), [2] TX bytes dropped because a ring was full,
 * [3] TX bytes handed to TinyUSB. */
extern volatile uint32_t  g_usb_stat[4];

/* A read of a SIO spinlock register *is* an acquire attempt.  Measured on an
 * RP2350: the answer is not "1 = mine" as the RP2040 datasheet's wording
 * suggests, it is one-hot - lock N answers with bit N set (read 31 and you get
 * 0x80000000, read 30 and you get 0x40000000).  A `>> 31` test therefore
 * declares every lock except 31 broken, which is exactly how an earlier build
 * "proved" the spinlocks unusable and booted the console unlocked.  Anything
 * non-zero means the caller now owns it, and it must be written back to 0
 * before the same core can take it again - a probe that forgets that leaves
 * the lock held for the rest of the boot. */
static uint32_t usb_lk_probe(uint32_t idx)
{
    volatile uint32_t *r = SIO_LK(idx);
    uint32_t v = *r;

    if (v)
        *r = 0;                             /* acquired by the probe: release */
    return v;
}

static void usb_lk_select(void)
{
    static const uint32_t cand[3] = { 30u, 29u, 28u };
    uint32_t i;

    /* 31 is the kernel's own gate.  Read it only to learn whether the SIO
     * lock window answers at all on this boot; released immediately. */
    (void)usb_lk_probe(31u);

    for (i = 0; i < 3u; i++) {
        uint32_t v = usb_lk_probe(cand[i]);

        if (v) {
            usb_lk_reg = SIO_LK(cand[i]);
            usb_lk_bit = v;             /* one-hot bit that means "mine" */
            return;
        }
    }
    usb_lk_reg = 0;
    g_usb_stat[1] = 1;      /* /proc/usbboot: this boot runs ungated */
    BM("[lk-none]");
}

/* TinyUSB state (CDC fifos, endpoint state, the device task) used to be
 * guarded by PRIMASK only - which is per core.  On an SMP build the console
 * writer on core 1 raced core 0's task-driven state machine: two cores
 * inside tu_fifo / tud at once corrupt the ring bookkeeping, and core 0
 * then wedged forever inside the state machine.  With interrupts masked
 * from sched_idle's console poll it stopped ticking, stopped answering the
 * flash park handshake, and every flash write from core 1 timed out with
 * -1.  Measured on atk-dnr2350am-smp: the mask probe pinned core 0 to the masked
 * section entered right before tty_poll (site 2, LR in sched_idle), frozen
 * for good.
 *
 * The SIO hardware spinlock is cross-core by construction.  Lock 30: 31 is
 * the kernel's spin gate.  PRIMASK around the lock keeps core 0's own USB
 * interrupt (priority 0, above SysTick) from pre-empting the core that
 * holds the lock - an ISR that spins on a lock held by the thread it just
 * pre-empted would never make progress.  Hold times are bounded: tud_task()
 * returns once the event queue is drained, and nothing inside these
 * sections waits on the other core.
 *
 * Two rules keep a single wedged path from spreading:
 *   - giving up does NOT drop the gate for good.  An earlier version set
 *     usb_lk_reg = 0 on the first timeout, so one unlucky section turned
 *     every later USB access on that boot into an unguarded one: silent
 *     FIFO corruption on the other core, not a visible hang.
 *   - every caller tests USB_LK_OK() and skips its work instead, so a
 *     timeout costs one dropped update, and /proc/usbboot counts them. */
static inline int usb_lk_claim(void)
{
    if (!(*usb_lk_reg & usb_lk_bit))
        return 0;
    __asm__ volatile("dmb" ::: "memory");
    return 1;
}

uint32_t usb_cdc_trylock(void)
{
    uint32_t id = (uint32_t)plat_cpu_id() & 1u;
    uint32_t pm;

    /* Nested section on the SAME core: the hardware lock is not recursive and
     * the outer section already masked interrupts, so just bump a depth
     * counter.  Without this, any path that reaches the console from inside
     * tud_task()/dcd_int_handler() (panic, debug print, a CDC callback on
     * another port) spins on a lock this very core is holding - a permanent
     * hang with interrupts masked, which is indistinguishable from a dead
     * board. */
    if (usb_depth[id]) {
        usb_depth[id]++;
        return USB_LK_NESTED;
    }

    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(pm) :: "memory");

    /* Boot found no spinlock this board would hand out: run without one. */
    if (!usb_lk_reg)
        return pm | USB_LK_NONE;

    if (!usb_lk_claim())
        return pm | USB_LK_FAILED;
    usb_depth[id] = 1;
    return pm;
}

uint32_t usb_cdc_lock(void)
{
    uint32_t id  = (uint32_t)plat_cpu_id() & 1u;
    uint32_t pm;
    uint32_t n;

    if (usb_depth[id]) {
        usb_depth[id]++;
        return USB_LK_NESTED;
    }

    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(pm) :: "memory");

    if (!usb_lk_reg)
        return pm | USB_LK_NONE;

    US(8);
    for (n = 0; ; n++) {
        if (usb_lk_claim()) {
            usb_depth[id] = 1;
            US(9);
            return pm;
        }
        if (n >= USB_LK_SPINS) {
#if TOS_DIAG
            BM("[ULK!]");
            plat_boot_mark_u32(id);
#endif
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
        return;                         /* outer section still owns it */
    }
    if (tok & (USB_LK_NONE | USB_LK_FAILED)) {
        __asm__ volatile("msr primask, %0" :: "r"(tok & 1u) : "memory");
        return;
    }
    usb_depth[id] = 0;
    __asm__ volatile("dmb" ::: "memory");
    if (usb_lk_reg)
        *usb_lk_reg = 0;
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
static volatile int g_in_task;

/* Bytes the state machine has completed, waiting for the console.
 *
 * tud_cdc_read_char() is TinyUSB code, and the console poll reaches the read
 * path with interrupts masked (tty_poll, from sched_idle and from the block
 * device while a task waits for flash).  Nothing in that path may wait: it has
 * no tick to fall back on and the other core may hold the USB lock for a whole
 * state-machine pass.  So the task - which already runs here, under the lock,
 * on core 0 - moves the bytes out into this ring, and the masked reader only
 * pops a byte array.  Both the fill and the pop happen on core 0; the reader
 * runs masked, so the fill cannot be running at the same time.
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
 * putc is called from the tick, from the echo path and from a fault handler -
 * always with interrupts masked, sometimes on the core that is already inside
 * an exception.  It must not take the USB lock (the other core can hold it for
 * a whole state-machine pass - the SMP console deadlock) and it must not enter
 * TinyUSB at all.
 *
 * One ring per core makes each producer/consumer pair single-ended even in the
 * SMP build: a core appends and advances only its own head, the pump (under
 * the lock, on core 0) is the only reader.  The head is published with a
 * barrier after the byte is stored, and the drain works from a snapshot, so a
 * producer that keeps writing - including putc called from inside the pump,
 * e.g. a panic print in a TinyUSB callback - only ever defers bytes to the
 * next pass. */
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

/* Move queued bytes into TinyUSB.  Lock held, core 0.  Flushing here is what
 * puts a packet on the wire; tud_cdc_write_flush() only hands what is queued
 * to the endpoint, it never waits for the host, so this stays bounded. */
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
    /* Flush what this pass queued, and anything a previous pass left in
     * TinyUSB's own buffer (that is the old poll-time flush). */
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

    US(5);
    if (from_isr)
        tud_task_ext(0, true);
    else
        tud_task();
    US(6);

    usb_rxq_fill();
    usb_txq_drain();

    g_in_task = 0;
    usb_cdc_unlock(primask);
}

/* IRQ 14, called from the shared vector table through g_usb_irq_handler.
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
 * arrive while the interrupt is briefly masked still get processed.
 *
 * On ARM it is also the board's deferred-work hook (g_idle_poll), run from
 * PendSV on every tick that interrupted user code and from the idle loop.
 * That is the boundary the platform contract calls thread context; without it
 * the pump only ran when nothing was runnable, and a task that kept the CPU
 * busy stalled the TX endpoint after about two packets (issue #46). */
void usb_cdc_poll(void)
{
    uint32_t primask;

    if (!g_hw_ready)
        return;

    /* Fallback for an interrupt that is pending but was never taken.  Seen on
     * hardware: INTS holds BUS_RESET|SETUP_REQ|SUSPEND with the NVIC line
     * enabled and PRIMASK clear, yet no further handler entries.  Servicing
     * the controller from the tick keeps enumeration alive in that state
     * instead of hanging forever; when the interrupt works normally this
     * reads zero and costs one register access per millisecond. */
    US(1);
    primask = usb_cdc_lock();
    US(2);
    if (USB_LK_OK(primask) && usb_hw->ints)
        dcd_int_handler(0);         /* still latched: retried next pass */
    US(3);
    usb_cdc_unlock(primask);

    US(4);
    usb_task_run(0);

    US(7);
    primask = usb_cdc_lock();
    if (USB_LK_OK(primask))
        usb_txq_drain();
    usb_cdc_unlock(primask);
    US(0);
}

/* ---- console API -------------------------------------------------------- */

void usb_cdc_putc(char c)
{
    /* Nothing is written before the host has configured the port: a
     * "buffer it anyway" variant made the RP2040 stop mid-boot on the UART
     * too, because every console byte then took the cross-core USB lock and
     * pushed into a FIFO that could not drain until enumeration.  Dropping
     * is the contract here - see kernel/plat/console.h: putc may drop, and
     * blocking on a host would stall every other core's output with it. */
    if (!g_hw_ready || !tud_mounted())
        return;
    US(10);
    /* No lock, no TinyUSB, no waiting: append to this core's ring and let
     * the pump move it out (see usb_txq_put).  A full ring drops the byte
     * and counts it; a caller that must not lose output gets a short write()
     * and retries. */
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
    /* What this core's ring can still take, not TinyUSB's free space: reading
     * the state machine's FIFO without the lock is exactly what this design
     * avoids, and the two differ by at most one pump pass. */
    return (int)((USB_TX_Q - 1u) -
                 ((usb_txq_head[id] + USB_TX_Q - usb_txq_tail[id]) % USB_TX_Q));
}

void usb_cdc_flush(void)
{
    uint32_t tok;

    if (!g_hw_ready)
        return;
    US(11);
    /* Callers include the panic path, which runs masked: take the section
     * only if it is free.  When the other core holds it the bytes are not
     * lost, they go out on the pump's next pass (PendSV/tick, 1 ms).  Waiting
     * here is the deadlock this driver was rewritten to remove. */
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

/* The chip's own 64-bit id, not a shared constant.
 *
 * Every board used to send "0001", so two boards on one host were the same
 * USB device as far as Windows was concerned: it moved them onto different
 * COM numbers and could hand a port to the wrong board.  The boot ROM reports
 * the id through SYS_INFO_CHIP_INFO ('G','S'), which is also where pico-sdk's
 * pico_unique_id gets it; the last 8 bytes of the 32-byte answer are the id,
 * little-endian. */
static char g_serial[17];

static void serial_from_chip_id(void)
{
    static const char hex[] = "0123456789abcdef";
    uint32_t info[9] = {0};
    void *(*lookup)(uint32_t, uint32_t) =
        (void *(*)(uint32_t, uint32_t))(uintptr_t)
            (*(volatile uint16_t *)BOOTROM_LOOKUP_PTR);
    int (*sysinfo)(uint32_t *, uint32_t, uint32_t) = 0;

    if (lookup)
        sysinfo = (int (*)(uint32_t *, uint32_t, uint32_t))(uintptr_t)
            ((uintptr_t)lookup(0x5347u, RT_FLAG_FUNC_ARM_SEC) | 1u);
    if (!sysinfo || sysinfo(info, 9, 0x0001u) != 4) {
        memcpy(g_serial, "0000000000000000", sizeof(g_serial));
        return;
    }
    for (int i = 0; i < 8; i++) {
        uint8_t v = ((const uint8_t *)info)[15 - i];

        g_serial[2 * i] = hex[v >> 4];
        g_serial[2 * i + 1] = hex[v & 0x0fu];
    }
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static char const *const str_table[] = {
        "TinyOS",                   /* iManufacturer */
        "TinyOS CDC-ACM",           /* iProduct      */
        g_serial,                   /* iSerialNumber: the chip's own id */
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
