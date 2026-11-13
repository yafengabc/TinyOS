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
#include "string.h"
#include "console.h"

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
    uint32_t primask;

    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");
    if (g_in_task) {
        __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
        return;                     /* the outer call will drain the queue */
    }
    g_in_task = 1;
    __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");

    if (from_isr)
        tud_task_ext(0, true);
    else
        tud_task();

    g_in_task = 0;
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
 * arrive while the interrupt is briefly masked still get processed. */
void usb_cdc_poll(void)
{
    if (!g_hw_ready)
        return;

    /* Fallback for an interrupt that is pending but was never taken.  Seen on
     * hardware: INTS holds BUS_RESET|SETUP_REQ|SUSPEND with the NVIC line
     * enabled and PRIMASK clear, yet no further handler entries.  Servicing
     * the controller from the tick keeps enumeration alive in that state
     * instead of hanging forever; when the interrupt works normally this
     * reads zero and costs one register access per millisecond. */
    if (usb_hw->ints)
        dcd_int_handler(0);

    usb_task_run(0);
    if (tud_cdc_write_available() < CFG_TUD_CDC_TX_BUFSIZE)
        tud_cdc_write_flush();
}

/* ---- console API -------------------------------------------------------- */

void usb_cdc_putc(char c)
{
    uint32_t primask;

    if (!g_hw_ready || !tud_mounted())
        return;

    /* Drop rather than block: putc is called from the tick and from the
     * console echo path, and a full FIFO there must not stall the kernel.
     * usb_cdc_tx_room() lets callers pace themselves instead.
     *
     * The old guard ("skip in exception context" via IPSR) was wrong: the
     * shell writes through the SVC syscall handler, which IS an exception,
     * so every byte the shell ever printed was silently dropped and the
     * CDC port never said a word after the boot banner.  The real hazard -
     * re-entrant FIFO access while tud_task() runs elsewhere - is covered
     * by this PRIMASK window instead: tu_fifo's write path is a handful of
     * instructions, and blocking everything for those beats an ipsr check
     * that cannot tell SVC from SysTick. */
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");
    if (tud_cdc_write_available() != 0)
        tud_cdc_write_char(c);
    __asm__ volatile("msr primask, %0" :: "r"(primask) : "memory");
}

int usb_cdc_tx_room(void)
{
    if (!g_hw_ready)
        return 0;
    return (int)tud_cdc_write_available();
}

void usb_cdc_flush(void)
{
    if (!g_hw_ready)
        return;
    tud_cdc_write_flush();
}

int usb_cdc_getchar(void)
{
    if (!g_hw_ready)
        return -1;
    int32_t c = tud_cdc_read_char();
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
