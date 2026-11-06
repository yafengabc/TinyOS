#ifndef _HARDWARE_REGS_USB_H
#define _HARDWARE_REGS_USB_H

/* TinyOS shim: only the USB register *bit* definitions the RP2040 port of
 * TinyUSB actually references, copied verbatim from pico-sdk's generated
 * hardware/regs/usb.h for RP2350 (SDK 2.3.1).  Keeping the values copied
 * rather than retyped means they cannot drift from the datasheet.
 *
 * The full pico-sdk header has ~4000 lines of Doxygen comments and host-mode
 * registers; this port only builds the device stack. */

#include "hardware/address_mapped.h"

// ---- device address -------------------------------------------------------
#define USB_ADDR_ENDP_OFFSET _u(0x00000000)
#define USB_ADDR_ENDP1_INTEP_PREAMBLE_BITS _u(0x04000000)
#define USB_ADDR_ENDP1_INTEP_DIR_BITS _u(0x02000000)

// ---- main control ---------------------------------------------------------
#define USB_MAIN_CTRL_CONTROLLER_EN_BITS _u(0x00000001)
#define USB_MAIN_CTRL_HOST_NDEVICE_BITS _u(0x00000002)
#define USB_MAIN_CTRL_PHY_ISO_BITS _u(0x00000004)

// ---- SIE control ----------------------------------------------------------
#define USB_SIE_CTRL_EP0_INT_1BUF_BITS _u(0x20000000)
#define USB_SIE_CTRL_PULLUP_EN_BITS _u(0x00010000)
#define USB_SIE_CTRL_PULLDOWN_EN_BITS _u(0x00008000)
#define USB_SIE_CTRL_RESUME_BITS _u(0x00001000)
#define USB_SIE_CTRL_SOF_EN_BITS _u(0x00000200)
#define USB_SIE_CTRL_KEEP_ALIVE_EN_BITS _u(0x00000400)
#define USB_SIE_CTRL_PREAMBLE_EN_BITS _u(0x00000040)
#define USB_SIE_CTRL_SEND_SETUP_BITS _u(0x00000002)
#define USB_SIE_CTRL_SEND_DATA_BITS _u(0x00000004)
#define USB_SIE_CTRL_RECEIVE_DATA_BITS _u(0x00000008)
#define USB_SIE_CTRL_START_TRANS_BITS _u(0x00000010)

// ---- SIE status -----------------------------------------------------------
#define USB_SIE_STATUS_SUSPENDED_BITS _u(0x00000010)
#define USB_SIE_STATUS_RESUME_BITS _u(0x00000800)
#define USB_SIE_STATUS_CONNECTED_BITS _u(0x00010000)
#define USB_SIE_STATUS_SETUP_REC_BITS _u(0x00020000)
#define USB_SIE_STATUS_BUS_RESET_BITS _u(0x00080000)
#define USB_SIE_STATUS_STALL_REC_BITS _u(0x20000000)
#define USB_SIE_STATUS_TRANS_COMPLETE_BITS _u(0x00040000)
#define USB_SIE_STATUS_SPEED_BITS _u(0x0c000000)
#define USB_SIE_STATUS_DATA_SEQ_ERROR_BITS _u(0x40000000)
#define USB_SIE_STATUS_RX_TIMEOUT_BITS _u(0x80000000)

// ---- interrupt routing ----------------------------------------------------
#define USB_INTE_OFFSET _u(0x00000090)
#define USB_INTF_OFFSET _u(0x00000094)
#define USB_INTS_OFFSET _u(0x00000098)

#define USB_INTS_ERROR_BIT_STUFF_BITS _u(0x00000100)
#define USB_INTS_ERROR_CRC_BITS _u(0x00000200)
#define USB_INTS_ERROR_DATA_SEQ_BITS _u(0x00000020)
#define USB_INTS_ERROR_RX_OVERFLOW_BITS _u(0x00000080)
#define USB_INTS_ERROR_RX_TIMEOUT_BITS _u(0x00000040)
#define USB_INTE_BUFF_STATUS_BITS _u(0x00000010)
#define USB_INTE_STALL_BITS _u(0x00002000)
#define USB_INTE_TRANS_COMPLETE_BITS _u(0x00040000)
#define USB_INTE_HOST_CONN_DIS_BITS _u(0x00002000)
#define USB_INTE_HOST_RESUME_BITS _u(0x00008000)
#define USB_INTE_ERROR_DATA_SEQ_BITS _u(0x00000020)
#define USB_INTE_ERROR_RX_TIMEOUT_BITS _u(0x00000040)

#define USB_INTS_BUFF_STATUS_BITS _u(0x00000010)
#define USB_INTS_BUS_RESET_BITS _u(0x00001000)
#define USB_INTS_DEV_CONN_DIS_BITS _u(0x00002000)
#define USB_INTS_DEV_SUSPEND_BITS _u(0x00004000)
#define USB_INTS_DEV_RESUME_FROM_HOST_BITS _u(0x00008000)
#define USB_INTS_DEV_SOF_BITS _u(0x00020000)
#define USB_INTS_SETUP_REQ_BITS _u(0x00010000)
#define USB_INTS_STALL_BITS _u(0x00002000)
#define USB_INTS_TRANS_COMPLETE_BITS _u(0x00040000)
#define USB_INTF_DEV_SOF_BITS _u(0x00020000)

// ---- endpoint control -----------------------------------------------------
#define USB_EP_ABORT_EP0_IN_BITS _u(0x00000001)
#define USB_EP_ABORT_EP0_OUT_BITS _u(0x00000002)
#define USB_EP_STALL_ARM_EP0_IN_BITS _u(0x00000001)
#define USB_EP_STALL_ARM_EP0_OUT_BITS _u(0x00000002)

// ---- PHY / power ----------------------------------------------------------
#define USB_USB_MUXING_TO_PHY_BITS _u(0x00000001)
#define USB_USB_MUXING_SOFTCON_BITS _u(0x00000008)
#define USB_USB_PWR_VBUS_DETECT_BITS _u(0x00000004)
#define USB_USB_PWR_VBUS_DETECT_OVERRIDE_EN_BITS _u(0x00000008)

#define USB_SOF_RD_BITS _u(0x000007ff)

#endif
