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
 *   +0x08/10/14      INT_RAW / INT_ENA / INT_CLR
 *                    SERIAL_OUT_RECV_PKT=2 (packet arrived),
 *                    SERIAL_IN_EMPTY=3 (host took the packet)
 *   +0x18 CONF0      PHY_SEL=0 (internal), DP_PULLUP=9, USB_PAD_ENABLE=14
 *
 * Both directions are interrupt driven.
 *
 * RX: SERIAL_OUT_RECV_PKT fires the moment a packet lands in the FIFO; the
 * trap handler drains it and wakes the blocked reader.  While the FIFO
 * holds an undrained packet the hardware NAKs the next OUT token, so a
 * fast host is throttled by the wire instead of losing bytes.
 *
 * TX: a 4 KiB ring filled by usj_putc() and drained by the SERIAL_IN_EMPTY
 * interrupt.  This used to be a blocking write with a 40000-iteration
 * timeout per byte, and with nothing plugged into the native USB port that
 * timeout *always* expired: every console byte burned microseconds with
 * interrupts off, which starved the UART0 receive drain and truncated
 * every incoming burst (measured: a 692-character line lost its tail, and
 * a transfer could not get past its first chunk).  Now the console never
 * waits for USB; with no host the ring fills up and drops, exactly like a
 * UART with a full transmitter.
 */

#include "board.h"              /* mmio_read32 / mmio_write32 */

#define USJ_BASE 0x60043000u

#define USJ_EP1         0x00u   /* FIFO: write byte, read byte            */
#define USJ_EP1_CONF    0x04u
#define USJ_WR_DONE        (1u << 0)
#define USJ_IN_DATA_FREE   (1u << 1)   /* TX FIFO has room (before flush) */
#define USJ_OUT_DATA_AVAIL (1u << 2)   /* RX FIFO holds a packet          */
#define USJ_INT_RAW     0x08u
#define USJ_INT_ENA     0x10u
#define USJ_INT_CLR     0x14u
#define USJ_INT_RECV_PKT   (1u << 2)   /* a packet arrived in the RX FIFO */
#define USJ_INT_IN_EMPTY   (1u << 3)   /* the host took our TX packet     */
#define USJ_CONF0       0x18u
#define USJ_DP_PULLUP      (1u << 9)
#define USJ_USB_PAD_ENABLE (1u << 14)

/* One USB packet's worth of payload: the FIFO takes 64 bytes, WR_DONE
 * commits them, and the next write has to wait for the host. */
#define USJ_PACKET 64

/* Transmit ring.  A power of two so the indices wrap with a mask.  Bigger
 * than one console line and bigger than the UART's own FIFO: the point is
 * that a program printing a screenful never finds the console full just
 * because nobody is plugged into the USB port. */
#define USJ_TXQ 4096
#define USJ_TXQ_MASK (USJ_TXQ - 1)

static char txq[USJ_TXQ];
static volatile unsigned tx_head, tx_tail;   /* producer / consumer */

void usj_init(void)
{
    uint32_t conf0 = mmio_read32(USJ_BASE + USJ_CONF0);
    conf0 |= USJ_DP_PULLUP | USJ_USB_PAD_ENABLE;
    mmio_write32(USJ_BASE + USJ_CONF0, conf0);
}

/* Queue one byte.  Never blocks: when the host is absent (or slower than
 * the program) the ring fills and the byte is dropped, the same trade the
 * UART transmitter makes when it is full.  Called from task context and
 * from the receive interrupt (keystroke echo), never from both at once -
 * interrupts are off inside a trap, and the ring is single-producer as
 * long as the byte is stored before the index moves. */
void usj_putc(char c)
{
    unsigned next = (tx_head + 1) & USJ_TXQ_MASK;

    if (next == tx_tail)
        return;                       /* nobody is draining: drop */
    txq[tx_head] = c;
    __sync_synchronize();             /* byte visible before the index */
    tx_head = next;

    /* Ask for a transmit interrupt.  IN_EMPTY is a level ("the FIFO is
     * free"), so enabling it while the FIFO is empty fires at once; the
     * handler fills a packet and switches it off again when the ring runs
     * dry.  With no host the FIFO never frees, no interrupt arrives, and
     * nothing spins. */
    mmio_write32(USJ_BASE + USJ_INT_ENA, USJ_INT_RECV_PKT | USJ_INT_IN_EMPTY);
}

int usj_getchar(void)
{
    if (!(mmio_read32(USJ_BASE + USJ_EP1_CONF) & USJ_OUT_DATA_AVAIL))
        return -1;
    return (int)(mmio_read32(USJ_BASE + USJ_EP1) & 0xFFu);
}

/* ---- interrupts ------------------------------------------------------- */

/* Enable the receive interrupt.  Called from plat_timer_init(), after the
 * INTMTX has routed source 26 to its CPU line.  Only the packet bit goes
 * on here: INT_RAW's SERIAL_IN_EMPTY bit powers up set, and enabling that
 * one at boot would storm before a single byte is queued. */
void usj_irq_init(void)
{
    mmio_write32(USJ_BASE + USJ_INT_CLR, USJ_INT_RECV_PKT | USJ_INT_IN_EMPTY);
    mmio_write32(USJ_BASE + USJ_INT_ENA, USJ_INT_RECV_PKT);
}

/* Service one interrupt: move queued output into the FIFO, then
 * acknowledge.  The trap handler has already drained the receive side
 * through plat_console_getchar() before calling this, so nothing is left
 * to do for RECV_PKT but clear it - and a packet that lands between the
 * drain and this write re-latches the bit and fires again. */
void usj_irq_service(void)
{
    unsigned n = 0;

    while (tx_tail != tx_head && n < USJ_PACKET &&
           (mmio_read32(USJ_BASE + USJ_EP1_CONF) & USJ_IN_DATA_FREE)) {
        mmio_write32(USJ_BASE + USJ_EP1, (uint32_t)(uint8_t)txq[tx_tail]);
        tx_tail = (tx_tail + 1) & USJ_TXQ_MASK;
        n++;
    }
    if (n > 0)
        mmio_write32(USJ_BASE + USJ_EP1_CONF, USJ_WR_DONE);

    /* Ring empty: stop asking for transmit interrupts.  Leaving the bit
     * on would re-enter this handler for every idle USB frame. */
    if (tx_tail == tx_head)
        mmio_write32(USJ_BASE + USJ_INT_ENA, USJ_INT_RECV_PKT);

    mmio_write32(USJ_BASE + USJ_INT_CLR, USJ_INT_RECV_PKT | USJ_INT_IN_EMPTY);
}

/* Push what is queued straight at the FIFO, bounded: used on the way down
 * (panic, reboot, shutdown) where the interrupt may not get another turn.
 * With no host this gives up quickly instead of hanging the halt. */
void usj_flush(void)
{
    int spin = 200000;

    while (tx_tail != tx_head) {
        if (!(mmio_read32(USJ_BASE + USJ_EP1_CONF) & USJ_IN_DATA_FREE)) {
            if (--spin == 0)
                return;
            continue;
        }
        unsigned n = 0;
        while (tx_tail != tx_head && n < USJ_PACKET) {
            mmio_write32(USJ_BASE + USJ_EP1, (uint32_t)(uint8_t)txq[tx_tail]);
            tx_tail = (tx_tail + 1) & USJ_TXQ_MASK;
            n++;
        }
        mmio_write32(USJ_BASE + USJ_EP1_CONF, USJ_WR_DONE);
    }
    /* Last packet: give the host a moment to take it. */
    for (spin = 100000; spin > 0; spin--)
        if (mmio_read32(USJ_BASE + USJ_EP1_CONF) & USJ_IN_DATA_FREE)
            break;
}
