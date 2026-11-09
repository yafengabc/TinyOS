/* Minimal USB CDC-ACM device stack for the RP2040, hand-rolled (no TinyUSB).
 *
 * Why hand-rolled: TinyUSB costs 12-18 KB of .text and this project's whole
 * point is extreme size minimization.  The RP2040 device controller is a simple
 * DPRAM-buffered SIE: a single-interface CDC-ACM with one interrupt-IN and two
 * bulk endpoints needs ~300 lines of C and lands around 2 KB.
 *
 * Integration with the kernel's polling console model: nothing here registers
 * an interrupt.  usb_cdc_poll() services every event the SIE can raise:
 *   - EP0 SETUP packets (enumeration + CDC class requests)
 *   - EP0 IN/OUT buffer completions (control data/status stages)
 *   - EP1 OUT completions (host -> device console input)
 *   - EP2 IN completions (device -> host console output freed)
 * It is called from putc/getchar/flush, i.e. at least once per SysTick tick
 * (100 Hz) and on every console write.  The USB bus is forgiving of lazy
 * servicing: the host simply receives NAKs on bulk endpoints and its control
 * transfer timeout is orders of magnitude longer than 10 ms.
 *
 * Register values are from the pico-sdk headers (hardware/regs/usb.h,
 * usb_device_dpram.h, hardware/structs/usb_dpram.h - BSD-3-Clause,
 * Raspberry Pi Ltd), cross-checked against the RP2040 datasheet section 4.1.
 */

#include "platform.h"
#include "board.h"
#include "arm_memmap.h"

/* ---- register block (USBCTRL_REGS_BASE 0x50110000) --------------------- */

#define USBR(off) (*(volatile uint32_t *)(0x50110000u + (off)))

#define USB_ADDR_ENDP     USBR(0x00)
#define USB_MAIN_CTRL     USBR(0x40)   /* CONTROLLER_EN=1, HOST_NDEVICE=0  */
#define USB_SIE_CTRL      USBR(0x4c)   /* PULLUP_EN = bit 16               */
#define USB_SIE_STATUS    USBR(0x50)
#define USB_BUFF_STATUS   USBR(0x58)   /* write-1-to-clear                 */
#define USB_EP_STALL_ARM  USBR(0x68)   /* EP0_IN=1, EP0_OUT=2              */
#define USB_USB_MUXING    USBR(0x74)   /* TO_PHY=1, TO_DIGITAL_PAD=4, SOFTCON=8 */
#define USB_USB_PWR       USBR(0x78)   /* VBUS_EN=1, VBUS_EN_OVR_EN=2,
                                        * VBUS_DETECT=4, VBUS_DETECT_OVR_EN=8   */

/* SIE_STATUS bits */
#define SIE_BUS_RESET     0x00080000u
#define SIE_SETUP_REC     0x00020000u
#define SIE_STALL_REC     0x20000000u

/* BUFF_STATUS bits (write-1-to-clear): one bit per endpoint buffer. */
#define BST_EP0_IN        0x00000001u
#define BST_EP0_OUT       0x00000002u
#define BST_EP1_IN        0x00000004u
#define BST_EP1_OUT       0x00000008u
#define BST_EP2_IN        0x00000010u
#define BST_EP2_OUT       0x00000020u

/* ---- DPRAM (USBCTRL_DPRAM_BASE 0x50100000) ----------------------------- */

#define DPRAM ((volatile uint8_t *)0x50100000u)

/* Setup packet lands here (bmRequestType, bRequest, wValue lo/hi,
 * wIndex lo/hi, wLength lo/hi). */
#define DP_SETUP          0x00u

/* Per-endpoint control registers at 0x08 + (n-1)*8 + dir*4 (n >= 1):
 * ENABLE is bit 31, TYPE is bits 27:26 (0 control, 2 bulk), the buffer
 * address sits at bits 15:6 with the low six bits ignored (64-byte
 * alignment).  INTERRUPT_PER_BUFFER (bit 29) is what makes a completion
 * visible in BUFF_STATUS for a driver that polls instead of interrupt. */
#define EP_CTRL_ENABLE      0x80000000u
#define EP_CTRL_INT_PER_BUF 0x20000000u
#define EP_CTRL_TYPE_BULK   (2u << 26)
#define DP_EPCTRL(ep, in) (0x08u + ((ep) - 1u) * 8u + ((in) ? 4u : 0u))

/* Per-endpoint buffer-control registers at 0x80 + ep*8 + dir*4, where
 * dir 0 = OUT, 1 = IN and EP0 uses the same layout (slot 0/1).  Single
 * (buffer 0) fields: */
#define BC_LEN_MASK       0x000003ffu  /* bytes (or max bytes for OUT)     */
#define BC_AVAIL          0x00000400u  /* arm the buffer                   */
#define BC_STALL          0x00000800u
#define BC_PID1           0x00002000u  /* 0 = DATA0, 1 = DATA1             */
#define BC_FULL           0x00008000u  /* IN: buffer holds data to send;
                                        * OUT: set by HW when data arrived */
#define DP_BUFCTRL(ep, in) (0x80u + (ep) * 8u + ((in) ? 4u : 0u))

/* Data areas: EP0 OUT at 0x100, EP0 IN at 0x140, then 0x180 onwards for
 * data endpoints. */
#define DP_EP0_OUT        0x100u
#define DP_EP0_IN         0x140u
#define DP_EP1_OUT_DATA   0x180u       /* bulk OUT (host -> device)        */
#define DP_EP2_IN_DATA    0x1c0u       /* bulk IN  (device -> host)        */

/* ---- reset / clock registers ------------------------------------------- */

#define RESETS_USBCTRL    0x01000000u
#define RESETS_PLL_USB    0x00002000u

#define PLL_USB_BASE      0x4002c000u
#define PLL_CS(off)       (*(volatile uint32_t *)(PLL_USB_BASE + (off)))
#define PLL_CS_REG        PLL_CS(0x00) /* LOCK = bit 31, REFDIV = 5:0      */
#define PLL_PWR_REG       PLL_CS(0x04) /* PD=1, POSTDIVPD=8, DSMPD=4, VCOPD=0x20 */
#define PLL_FBDIV_REG     PLL_CS(0x08)
#define PLL_PRIM_REG      PLL_CS(0x0c) /* POSTDIV1 = 18:16, POSTDIV2 = 14:12 */

/* clk_usb must be exactly 48 MHz.  12 MHz XOSC / 1 * 100 = 1200 MHz VCO
 * (inside the 750-1600 MHz range), / 5 / 5 = 48 MHz. */
#define PLL_REFDIV        1u
#define PLL_FBDIV         100u
#define PLL_POSTDIV1      5u
#define PLL_POSTDIV2      5u

/* The divider is part of the same write as ENABLE: INT resets to 0, and a
 * divisor of 0 is not "pass through" - clk_usb would not run and the
 * controller would never see a 48 MHz clock.  INT = 1 with the PLL already
 * at 48 MHz gives the 48 MHz the PHY needs.  AUXSRC = 0 is clksrc_pll_usb. */
#define CLOCKS_CLK_USB_CTRL (*(volatile uint32_t *)(0x40008000u + 0x54u))
#define CLK_USB_ENABLE      0x00000800u
#define CLK_USB_DIV_INT_1   (1u << 16)
#define CLK_USB_AUX_PLL_USB 0x00000000u /* AUXSRC: clksrc_pll_usb           */

/* ---- descriptors -------------------------------------------------------- */

/* VID/PID: Raspberry Pi vendor with a TinyOS-specific product id.  Windows
 * binds usbser.sys (COM port) to CDC-ACM automatically on Win10+. */
#define USB_VID           0x2e8au
#define USB_PID           0x000au

static const uint8_t dev_desc[18] = {
    18, 0x01, 0x10, 0x01,     /* bcdUSB 1.10 (full speed)                */
    0x00, 0x00, 0x00,         /* class/subclass/protocol: per-interface  */
    64,                       /* bMaxPacketSize0                         */
    (uint8_t)USB_VID, (uint8_t)(USB_VID >> 8),
    (uint8_t)USB_PID, (uint8_t)(USB_PID >> 8),
    0x01, 0x00,               /* bcdDevice 0.01                          */
    1, 2, 0, 1,               /* strings: product, none, none, product   */
};

/* Configuration: IAD + CDC control interface (int IN) + data interface
 * (bulk IN/OUT).  wTotalLength = 75. */
static const uint8_t cfg_desc[75] = {
    0x09, 0x02, 75, 0, 2, 1, 0, 0x80, 50,
    /* IAD: CDC control + data as one function */
    0x08, 0x0b, 0, 2, 2, 2, 0, 0,
    /* interface 0: CDC control */
    0x09, 0x04, 0, 0, 1, 2, 2, 1, 0,
    0x05, 0x24, 0, 0x10, 0x01,        /* header, bcdCDC 1.10             */
    0x04, 0x24, 0x02, 0x02,           /* ACM: SET_LINE_CODING etc.       */
    0x05, 0x24, 0x06, 0, 1,           /* union: master 0, slave 1        */
    0x05, 0x24, 0x01, 1,              /* call management: via data if 1  */
    0x07, 0x05, 0x81, 3, 8, 0, 10,    /* interrupt IN, 8 B, 10 ms        */
    /* interface 1: CDC data */
    0x09, 0x04, 1, 0, 2, 10, 0, 0, 0,
    0x07, 0x05, 0x02, 2, 64, 0, 0,    /* bulk OUT                        */
    0x07, 0x05, 0x82, 2, 64, 0, 0,    /* bulk IN                         */
};

static const uint8_t str_langid[4] = { 4, 0x03, 0x09, 0x04 };
static const uint8_t str_product[14] = {
    14, 0x03, 'T', 0, 'i', 0, 'n', 0, 'y', 0, 'O', 0, 'S', 0,
};

/* ---- state -------------------------------------------------------------- */

#define EP0_MAX_PKT 64u

/* EP0 stage machine: what to do when the currently armed EP0 buffer
 * completes. */
enum {
    EP0_IDLE = 0,
    EP0_IN_DATA,        /* sending descriptor bytes                        */
    EP0_IN_STATUS,      /* zero-length status IN sent, wait for it to land */
    EP0_OUT_DATA,       /* receiving OUT payload (SET_LINE_CODING)         */
    EP0_OUT_STATUS,     /* receiving zero-length status OUT                */
};

static uint8_t  ep0_stage;
static const uint8_t *ep0_tx;       /* remaining descriptor bytes          */
static uint32_t ep0_tx_len;
static uint32_t ep0_pending_addr;
static uint8_t  line_coding[7] = {   /* 115200 8N1                          */
    0x00, 0xc2, 0x01, 0x00,         /* dwDTERate = 115200                  */
    0x00,                           /* bCharFormat = 1 stop bit            */
    0x00,                           /* bParityType = none                  */
    0x08,                           /* bDataBits = 8                       */
};

static volatile int g_configured;
static int ep0_data1;               /* next EP0 data-stage PID             */
static int in_toggle, out_toggle;   /* bulk EP DATA0/1 alternation         */
static int in_armed, out_armed;

#define CDC_RING 512u
static volatile uint32_t rx_r, rx_w;
static volatile uint32_t tx_r, tx_w;
static uint8_t rx_ring[CDC_RING];
static uint8_t tx_ring[CDC_RING];

/* ---- low-level helpers --------------------------------------------------- */

static inline uint32_t bc_read(uint8_t slot)   { return *(volatile uint32_t *)(0x50100000u + slot); }
static inline void     bc_write(uint8_t slot, uint32_t v) { *(volatile uint32_t *)(0x50100000u + slot) = v; }

static void ep_arm_in(uint8_t slot, uint32_t data_off, const uint8_t *src,
                      uint32_t n, int pid1)
{
    volatile uint8_t *dst = DPRAM + data_off;
    for (uint32_t i = 0; i < n; i++)
        dst[i] = src[i];
    bc_write(slot, n | BC_FULL | BC_AVAIL | (pid1 ? BC_PID1 : 0));
}

static void ep_arm_out(uint8_t slot, uint32_t len, int pid1)
{
    bc_write(slot, len | BC_AVAIL | (pid1 ? BC_PID1 : 0));
}

/* STALL the control transfer (rejected request).  EP0 stalls must be armed
 * through EP_STALL_ARM first. */
static void ep0_stall(void)
{
    USB_EP_STALL_ARM = 3u;          /* EP0_IN | EP0_OUT: both directions */
    bc_write(DP_BUFCTRL(0, 1), BC_STALL);
    bc_write(DP_BUFCTRL(0, 0), BC_STALL);
}

/* Send one EP0 IN packet (<= 64 B) from ep0_tx/ep0_tx_len. */
static void ep0_send_packet(void)
{
    uint32_t n = ep0_tx_len;
    if (n > EP0_MAX_PKT)
        n = EP0_MAX_PKT;
    ep_arm_in(DP_BUFCTRL(0, 1), DP_EP0_IN, ep0_tx, n, ep0_data1);
    ep0_data1 ^= 1;
    ep0_tx += n;
    ep0_tx_len -= n;
    ep0_stage = EP0_IN_DATA;
}

/* Complete a control transfer: status stage is an IN zero-length packet. */
static void ep0_send_status(void)
{
    ep_arm_in(DP_BUFCTRL(0, 1), DP_EP0_IN, NULL, 0, 1);
    ep0_stage = EP0_IN_STATUS;
}

/* Receive the status OUT packet that ends a transfer with an IN data stage. */
static void ep0_expect_status_out(void)
{
    ep_arm_out(DP_BUFCTRL(0, 0), 0, 1);
    ep0_stage = EP0_OUT_STATUS;
}

/* ---- EP0 SETUP dispatch -------------------------------------------------- */

static void ep0_handle_setup(void)
{
    volatile uint8_t *s = DPRAM + DP_SETUP;
    uint8_t type    = s[0];
    uint8_t request = s[1];
    uint16_t wvalue = (uint16_t)(s[2] | (s[3] << 8));
    uint16_t wlen   = (uint16_t)(s[6] | (s[7] << 8));

    ep0_pending_addr = 0;
    ep0_data1 = 1;                  /* data stage restarts at DATA1     */

    if ((type & 0x60) == 0x00) {    /* standard                         */
        switch (request) {
        case 0x05:                  /* SET_ADDRESS                      */
            ep0_pending_addr = wvalue & 0x7f;
            ep0_send_status();
            return;
        case 0x06:                  /* GET_DESCRIPTOR                   */
            switch (wvalue >> 8) {
            case 1:                 /* device                           */
                ep0_tx = dev_desc;
                ep0_tx_len = sizeof(dev_desc);
                break;
            case 2:                 /* configuration (all descriptors)  */
                ep0_tx = cfg_desc;
                ep0_tx_len = sizeof(cfg_desc);
                break;
            case 3:                 /* string                           */
                if ((wvalue & 0xff) == 0) {
                    ep0_tx = str_langid;
                    ep0_tx_len = sizeof(str_langid);
                } else {
                    ep0_tx = str_product;
                    ep0_tx_len = sizeof(str_product);
                }
                break;
            default:
                ep0_stall();        /* device_qualifier (FS) etc.       */
                return;
            }
            if (ep0_tx_len > wlen)
                ep0_tx_len = wlen;
            ep0_send_packet();
            return;
        case 0x08:                  /* GET_STATUS (device): self-powered,
                                     * no remote wakeup                */
            ep_arm_in(DP_BUFCTRL(0, 1), DP_EP0_IN,
                      (const uint8_t *)"\0\0", 2, 1);
            ep0_stage = EP0_IN_DATA;
            ep0_tx = NULL;
            ep0_tx_len = 0;
            return;
        case 0x09:                  /* SET_CONFIGURATION                */
            g_configured = (wvalue & 0xff) ? 1 : 0;
            if (g_configured) {
                in_toggle = out_toggle = 0;
                in_armed = out_armed = 0;
                ep_arm_out(DP_BUFCTRL(1, 0), EP0_MAX_PKT, 0);
                out_armed = 1;
            }
            ep0_send_status();
            return;
        case 0x00:                  /* GET_STATUS (interface)           */
            if (type & 0x1f) {      /* to interface: two zero bytes     */
                ep_arm_in(DP_BUFCTRL(0, 1), DP_EP0_IN,
                          (const uint8_t *)"\0\0", 2, 1);
                ep0_stage = EP0_IN_DATA;
                ep0_tx = NULL;
                ep0_tx_len = 0;
                return;
            }
            break;
        }
    } else if (type == 0x21) {      /* CDC class -> interface           */
        switch (request) {
        case 0x20:                  /* SET_LINE_CODING: 7-byte OUT data */
            if (wlen == 7) {
                ep_arm_out(DP_BUFCTRL(0, 0), 7, 1);
                ep0_stage = EP0_OUT_DATA;
                return;
            }
            break;
        case 0x21:                  /* GET_LINE_CODING                  */
            ep_arm_in(DP_BUFCTRL(0, 1), DP_EP0_IN, line_coding, 7, 1);
            ep0_stage = EP0_IN_DATA;
            ep0_tx = NULL;
            ep0_tx_len = 0;
            return;
        case 0x22:                  /* SET_CONTROL_LINE_STATE (DTR/RTS) */
            ep0_send_status();
            return;
        }
    }

    ep0_stall();
}

/* ---- polled event pump ---------------------------------------------------- */

static void usb_poll(void)
{
    uint32_t st = USB_SIE_STATUS;

    /* Bus reset: forget everything, the host re-enumerates. */
    if (st & SIE_BUS_RESET) {
        USB_SIE_STATUS = SIE_BUS_RESET | SIE_STALL_REC;
        USB_ADDR_ENDP = 0;
        g_configured = 0;
        ep0_stage = EP0_IDLE;
        in_armed = out_armed = 0;
        tx_r = tx_w = 0;
        rx_r = rx_w = 0;
        return;
    }

    if (USB_BUFF_STATUS & BST_EP0_OUT) {
        USB_BUFF_STATUS = BST_EP0_OUT;
        uint32_t len = bc_read(DP_BUFCTRL(0, 0)) & BC_LEN_MASK;

        switch (ep0_stage) {
        case EP0_OUT_DATA:          /* SET_LINE_CODING payload          */
            if (len == 7) {
                volatile uint8_t *d = DPRAM + DP_EP0_OUT;
                for (uint32_t i = 0; i < 7; i++)
                    line_coding[i] = d[i];
            }
            ep0_send_status();
            break;
        case EP0_OUT_STATUS:        /* data-stage transfer complete     */
            if (ep0_pending_addr)
                USB_ADDR_ENDP = ep0_pending_addr;
            ep0_stage = EP0_IDLE;
            break;
        default:
            ep0_stage = EP0_IDLE;
            break;
        }
    }

    if (USB_BUFF_STATUS & BST_EP0_IN) {
        USB_BUFF_STATUS = BST_EP0_IN;
        switch (ep0_stage) {
        case EP0_IN_DATA:
            if (ep0_tx_len)
                ep0_send_packet();          /* multi-packet (config desc) */
            else
                ep0_expect_status_out();
            break;
        case EP0_IN_STATUS:
            if (ep0_pending_addr)           /* SET_ADDRESS takes effect   */
                USB_ADDR_ENDP = ep0_pending_addr;   /* after the status   */
            ep0_stage = EP0_IDLE;                   /* stage completes    */
            break;
        default:
            ep0_stage = EP0_IDLE;
            break;
        }
    }

    if (USB_SIE_STATUS & SIE_SETUP_REC) {
        USB_SIE_STATUS = SIE_SETUP_REC;
        ep0_handle_setup();
    }

    /* Bulk OUT (EP1): host sent console input. */
    if (USB_BUFF_STATUS & BST_EP1_OUT) {
        USB_BUFF_STATUS = BST_EP1_OUT;
        uint32_t len = bc_read(DP_BUFCTRL(1, 0)) & BC_LEN_MASK;
        if (len) {
            volatile uint8_t *d = DPRAM + DP_EP1_OUT_DATA;
            for (uint32_t i = 0; i < len; i++) {
                uint32_t next = (rx_w + 1u) & (CDC_RING - 1u);
                if (next == rx_r)
                    break;          /* ring full: drop (input, not data) */
                rx_ring[rx_w] = d[i];
                rx_w = next;
            }
        }
        ep_arm_out(DP_BUFCTRL(1, 0), EP0_MAX_PKT, out_toggle);
        out_toggle ^= 1;
    }

    /* Bulk IN (EP2): host drained the previous chunk. */
    if (USB_BUFF_STATUS & BST_EP2_IN) {
        USB_BUFF_STATUS = BST_EP2_IN;
        in_armed = 0;
    }
}

/* Move as much of the tx ring into the endpoint as possible.  Never blocks:
 * if the previous packet is still in flight or the host is not listening,
 * the bytes stay queued until a later tick. */
static void usb_tx_drain(void)
{
    if (!g_configured || in_armed)
        return;
    uint32_t count = (tx_w - tx_r) & (CDC_RING - 1u);
    if (!count)
        return;
    if (count > EP0_MAX_PKT)
        count = EP0_MAX_PKT;

    uint8_t pkt[EP0_MAX_PKT];
    for (uint32_t i = 0; i < count; i++) {
        pkt[i] = tx_ring[tx_r];
        tx_r = (tx_r + 1u) & (CDC_RING - 1u);
    }
    ep_arm_in(DP_BUFCTRL(2, 1), DP_EP2_IN_DATA, pkt, count, in_toggle);
    in_toggle ^= 1;
    in_armed = 1;
}

/* ---- public console API ---------------------------------------------------- */

void usb_cdc_init(void)
{
    uint32_t m;

    /* Bring the USB controller and its PLL out of reset. */
    m = mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET);
    m &= ~(RESETS_USBCTRL | RESETS_PLL_USB);
    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET, m);
    while (!(mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) &
             (RESETS_USBCTRL | RESETS_PLL_USB)))
        ;

    /* PLL USB: 12 MHz * 100 = 1200 MHz VCO, /5/5 = 48 MHz clk_usb. */
    PLL_CS_REG = PLL_REFDIV;
    PLL_FBDIV_REG = PLL_FBDIV;
    PLL_PRIM_REG = (PLL_POSTDIV1 << 16) | (PLL_POSTDIV2 << 12);
    PLL_PWR_REG = 0;                /* release PD, POSTDIVPD, VCOPD, DSMPD */
    while (!(PLL_CS_REG & 0x80000000u))
        ;                           /* wait for lock                       */

    CLOCKS_CLK_USB_CTRL = CLK_USB_DIV_INT_1 | CLK_USB_ENABLE | CLK_USB_AUX_PLL_USB;

    /* Device mode, PHY to internal pads, VBUS forced present.
     *
     * VBUS_DETECT_OVR_EN + VBUS_DETECT tell the controller "the host is
     * supplying VBUS" without sampling the pin.  VBUS_EN deliberately stays
     * clear: on a Pico the VBUS net is the host's own 5 V, and driving it
     * back into the cable is wrong (the SDK does not set it either). */
    USB_USB_MUXING = 0x01u | 0x08u;             /* TO_PHY | SOFTCON      */
    USB_USB_PWR = 0x04u | 0x08u;                /* VBUS_DETECT | OVR_EN  */
    USB_MAIN_CTRL = 0x01u;                      /* CONTROLLER_EN         */

    /* Data endpoints: EP1 OUT (rx) and EP2 IN (tx), 64-byte buffers.
     *
     * TYPE has to be BULK (2) at bits 27:26 - a freshly reset endpoint is
     * type CONTROL, and a bulk transfer through a control-type endpoint is
     * not handshaken the way the host expects.  INTERRUPT_PER_BUFFER (bit
     * 29) makes the SIE report each buffer completion in BUFF_STATUS, which
     * is the only event source this polled driver has.
     *
     * BUFFER_ADDRESS is the buffer's DPRAM offset: the field holds bits
     * 15:6 and the low six address bits are ignored, so writing the
     * (64-byte aligned) offset itself is correct. */
    *(volatile uint32_t *)(0x50100000u + DP_EPCTRL(1, 0)) =
        EP_CTRL_ENABLE | EP_CTRL_INT_PER_BUF | EP_CTRL_TYPE_BULK | DP_EP1_OUT_DATA;
    *(volatile uint32_t *)(0x50100000u + DP_EPCTRL(2, 1)) =
        EP_CTRL_ENABLE | EP_CTRL_INT_PER_BUF | EP_CTRL_TYPE_BULK | DP_EP2_IN_DATA;

    USB_ADDR_ENDP = 0;
    USB_SIE_CTRL = 0x00010000u;     /* PULLUP_EN: host sees us now        */
}

/* Wait briefly for the host to enumerate, so early output (banner, prompt)
 * is not dropped.  Bounded: at 12 MHz one spin is about 130 cycles, so
 * 150000 spins is roughly 1.6 s - the host is supposed to start talking
 * within 100 ms of the pull-up, and a longer wait only delays a board that
 * will never be enumerated (no cable, or a host that is asleep). */
void usb_cdc_wait(void)
{
    uint32_t spins = 0;
    while (!g_configured && spins < 150000u) {
        usb_poll();
        spins++;
        for (volatile int i = 0; i < 40; i++)
            ;
    }
}

void usb_cdc_console_init(void)
{
    usb_cdc_init();
    usb_cdc_wait();
}

int usb_cdc_configured(void)
{
    return g_configured;
}

int usb_cdc_tx_room(void)
{
    uint32_t m, room;

    __asm__ volatile("mrs %0, primask" : "=r"(m));
    __asm__ volatile("cpsid i" ::: "memory");
    room = (tx_r - tx_w - 1u) & (CDC_RING - 1u);
    if (!(m & 1u))
        __asm__ volatile("cpsie i" ::: "memory");
    return (int)room;
}

void usb_cdc_putc(char c)
{
    uint32_t m;

    __asm__ volatile("mrs %0, primask" : "=r"(m));
    __asm__ volatile("cpsid i" ::: "memory");
    uint32_t next = (tx_w + 1u) & (CDC_RING - 1u);
    if (next != tx_r) {
        tx_ring[tx_w] = (uint8_t)c;
        tx_w = next;
    } else {
        usb_poll();                 /* ring full: try to drain one packet */
        next = (tx_w + 1u) & (CDC_RING - 1u);
        if (next != tx_r) {
            tx_ring[tx_w] = (uint8_t)c;
            tx_w = next;
        }
        /* else: drop.  Output before enumeration is unavoidably lost.     */
    }
    if (!(m & 1u))
        __asm__ volatile("cpsie i" ::: "memory");

    usb_poll();
    usb_tx_drain();
}

void usb_cdc_flush(void)
{
    uint32_t spins = 0;
    while (tx_r != tx_w && spins < 200000u) {
        usb_poll();
        usb_tx_drain();
        spins++;
        for (volatile int i = 0; i < 20; i++)
            ;
    }
}

int usb_cdc_getchar(void)
{
    usb_poll();
    if (rx_r == rx_w)
        return -1;
    int c = rx_ring[rx_r];
    rx_r = (rx_r + 1u) & (CDC_RING - 1u);
    return c;
}
