/* ESP32-C3 USB-Serial/JTAG console (hardware CDC).
 *
 * The C3 has a fixed-function USB CDC device (the same block the ROM download
 * mode uses): no descriptors to serve, no protocol stack to write - enabling
 * the PHY pads and the D+ pull-up makes the host enumerate a COM port, and
 * the console then just moves bytes through the EP1 FIFO.
 *
 * Register layout from esp-idf soc/usb_serial_jtag_reg.h (Apache-2.0):
 *   +0x00 EP1        TX/RX FIFO (low 8 bits)
 *   +0x04 EP1_CONF   WR_DONE=0, SERIAL_IN_EP_DATA_FREE=1,
 *                    SERIAL_OUT_EP_DATA_AVAIL=2
 *   +0x18 CONF0      PHY_SEL=0 (internal), DP_PULLUP=9, USB_PAD_ENABLE=14
 *
 * Polled, non-blocking, and never an interrupt source: output is dropped
 * when the host is not reading (the same trade-off the RP2040 CDC stack
 * makes), input is picked up by the tick-driven tty poll.
 */

#include "board.h"              /* mmio_read32 / mmio_write32 */

#define USJ_BASE 0x60043000u

#define USJ_EP1         0x00u   /* FIFO: write byte, read byte            */
#define USJ_EP1_CONF    0x04u
#define USJ_WR_DONE        (1u << 0)
#define USJ_IN_DATA_FREE   (1u << 1)   /* TX FIFO has room (before flush) */
#define USJ_OUT_DATA_AVAIL (1u << 2)   /* RX FIFO holds a packet          */
#define USJ_CONF0       0x18u
#define USJ_DP_PULLUP      (1u << 9)
#define USJ_USB_PAD_ENABLE (1u << 14)

void usj_init(void)
{
    uint32_t conf0 = mmio_read32(USJ_BASE + USJ_CONF0);
    conf0 |= USJ_DP_PULLUP | USJ_USB_PAD_ENABLE;
    mmio_write32(USJ_BASE + USJ_CONF0, conf0);
}

/* Send one byte if the host is draining; otherwise drop it.  WR_DONE
 * commits the FIFO content as one USB packet; IN_DATA_FREE goes 0 until
 * the host reads it, which is our natural flow control. */
void usj_putc(char c)
{
    uint32_t conf = mmio_read32(USJ_BASE + USJ_EP1_CONF);
    if (!(conf & USJ_IN_DATA_FREE))
        return;
    mmio_write32(USJ_BASE + USJ_EP1, (uint32_t)(uint8_t)c);
    mmio_write32(USJ_BASE + USJ_EP1_CONF, USJ_WR_DONE);
}

int usj_getchar(void)
{
    if (!(mmio_read32(USJ_BASE + USJ_EP1_CONF) & USJ_OUT_DATA_AVAIL))
        return -1;
    return (int)(mmio_read32(USJ_BASE + USJ_EP1) & 0xFFu);
}
