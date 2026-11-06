#ifndef _HARDWARE_STRUCTS_USB_H
#define _HARDWARE_STRUCTS_USB_H

/* TinyOS shim: the two register blocks TinyUSB's RP2040 device port touches,
 * laid out to the RP2350 datasheet (12.7.3.7).  Only the device-side fields
 * exist here; the host-mode registers between them are kept as reserved
 * words so the offsets stay exact rather than approximately right. */

#include "hardware/address_mapped.h"
#include "hardware/regs/usb.h"

#define USBCTRL_DPRAM_BASE _u(0x50100000)
#define USBCTRL_REGS_BASE  _u(0x50110000)
#define USBCTRL_IRQ        _u(14)

#define USB_NUM_ENDPOINTS  16
#define USB_MAX_ENDPOINTS  USB_NUM_ENDPOINTS
#define USB_DPRAM_SIZE     4096u
#define USB_DPRAM_MAX      USB_DPRAM_SIZE

/* usb_hw: the controller register file at USBCTRL_REGS_BASE. */
typedef struct {
    io_rw_32 dev_addr_ctrl;             // 0x000 ADDR_ENDP
    io_rw_32 int_ep_addr_ctrl[15];      // 0x004 ADDR_ENDP1..15
    io_rw_32 main_ctrl;                 // 0x040
    io_rw_32 sof_wr;                    // 0x044
    io_rw_32 sof_rd;                    // 0x048
    io_rw_32 sie_ctrl;                  // 0x04c
    io_rw_32 sie_status;                // 0x050
    io_rw_32 int_ep_ctrl;               // 0x054
    io_rw_32 buf_status;                // 0x058 BUFF_STATUS
    io_rw_32 buff_cpu_should_handle;    // 0x05c
    io_rw_32 abort;                     // 0x060 EP_ABORT
    io_rw_32 abort_done;                // 0x064 EP_ABORT_DONE
    io_rw_32 ep_stall_arm;              // 0x068
    io_rw_32 _reserved0;                // 0x06c
    io_rw_32 ep_status_stall_nak;       // 0x070
    io_rw_32 muxing;                    // 0x074 USB_MUXING
    io_rw_32 pwr;                       // 0x078 USB_PWR
    io_rw_32 phy_direct;                // 0x07c USBPHY_DIRECT
    io_rw_32 phy_direct_override;       // 0x080
    io_rw_32 phy_trim;                  // 0x084
    io_rw_32 _reserved1[2];             // 0x088..0x08f
    io_rw_32 inte;                      // 0x090
    io_rw_32 intf;                      // 0x094
    io_rw_32 ints;                      // 0x098
} usb_hw_t;

/* usb_dpram: the 4 KB packet buffer + per-endpoint control words that live
 * at USBCTRL_DPRAM_BASE.  sizeof() must stay exactly 4096 because
 * rp2040_usb_init() clears the whole block with one memset. */
typedef struct {
    uint8_t setup_packet[8];            // 0x000, written by the SIE

    struct usb_device_dpram_ep_ctrl {   // starts at EP1, not EP0
        io_rw_32 in;
        io_rw_32 out;
    } ep_ctrl[USB_NUM_ENDPOINTS - 1];   // 0x008..0x07f

    struct usb_device_dpram_ep_buf_ctrl { // starts at EP0
        io_rw_32 in;
        io_rw_32 out;
    } ep_buf_ctrl[USB_NUM_ENDPOINTS];   // 0x080..0x0ff

    uint8_t ep0_buf_a[0x40];            // 0x100, shared by EP0 IN and OUT
    uint8_t ep0_buf_b[0x40];            // 0x140, only used in double-buffered mode
    uint8_t epx_data[USB_DPRAM_MAX - 0x180];
} usb_dpram_t;

/* Buffer control word bits (datasheet 12.7.3.7.2).  These are the values the
 * SIE interprets, not register bit positions, so they stay plain constants. */
#define USB_BUF_CTRL_FULL       0x00008000u  /* buffer holds data to send     */
#define USB_BUF_CTRL_LAST       0x00004000u  /* last buffer of the transfer   */
#define USB_BUF_CTRL_DATA0_PID  0x00000000u
#define USB_BUF_CTRL_DATA1_PID  0x00002000u
#define USB_BUF_CTRL_SEL        0x00001000u  /* reset buffer select to buf 0  */
#define USB_BUF_CTRL_STALL      0x00000800u
#define USB_BUF_CTRL_AVAIL      0x00000400u  /* hand the buffer to the SIE    */
#define USB_BUF_CTRL_LEN_MASK   0x000003FFu
#define USB_BUF_CTRL_LEN_LSB    0

/* Endpoint control word bits (datasheet 12.7.3.7.2, one per non-control
 * endpoint).  TYPE is 0 = control, 1 = isochronous, 2 = bulk, 3 = interrupt. */
#define EP_CTRL_ENABLE_BITS                  (1u << 31u)
#define EP_CTRL_DOUBLE_BUFFERED_BITS         (1u << 30)
#define EP_CTRL_INTERRUPT_PER_BUFFER         (1u << 29)
#define EP_CTRL_INTERRUPT_PER_DOUBLE_BUFFER  (1u << 28)
#define EP_CTRL_INTERRUPT_ON_STALL           (1u << 17u)
#define EP_CTRL_INTERRUPT_ON_NAK             (1u << 16u)
#define EP_CTRL_BUFFER_TYPE_LSB              26u
#define EP_CTRL_HOST_INTERRUPT_INTERVAL_LSB  16u

#define usb_hw     ((usb_hw_t *) USBCTRL_REGS_BASE)
#define usb_dpram  ((usb_dpram_t *) USBCTRL_DPRAM_BASE)

#endif
