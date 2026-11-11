#include "platform.h"
#include "board.h"
#include "arm_memmap.h"
#include "cpu.h"

#ifdef TOS_UART1

/* UART1 on GPIO4 (TX) / GPIO5 (RX), 115200 8N1.
 *
 * UART0 is the console and keeps its own polled driver in uart_dbg.c.  This
 * is the second PL011 instance at 0x40038000, exposed to user space as
 * port 0 through SYS_UART_*.  Both are fed by clk_peri; the console init
 * already enabled that clock, so only the UART1 reset and pin mux are new.
 *
 * Reads are non-blocking on purpose: there is no tty line discipline or
 * wait queue here, and a shell command that polls is enough for a loopback
 * test or a small protocol. */

#define UART1_DR    (UART1_BASE + 0x00u)
#define UART1_FR    (UART1_BASE + 0x18u)
#define UART1_IBRD  (UART1_BASE + 0x24u)
#define UART1_FBRD  (UART1_BASE + 0x28u)
#define UART1_LCR_H (UART1_BASE + 0x2cu)
#define UART1_CR    (UART1_BASE + 0x30u)
#define UART1_ICR   (UART1_BASE + 0x44u)

#define UART_FR_TXFF (1u << 5)
#define UART_FR_RXFE (1u << 4)

#define UART1_TX_PIN 4
#define UART1_RX_PIN 5

#define IO_BANK0_GPIO_CTRL(n) (IO_BANK0_BASE + 0x04u + 8u * (uint32_t)(n))
#define PADS_BANK0_GPIO(n)    (PADS_BANK0_BASE + 0x04u + 4u * (uint32_t)(n))

#define PAD_OD  (1u << 7)
#define PAD_IE  (1u << 6)
#define PAD_PUE (1u << 3)
#define PAD_PDE (1u << 2)
#define PAD_ISO (1u << 8)
#define FUNCSEL_UART 2u

static spin_t uart1_lock = SPIN_INIT;
static int uart1_ready;

static void uart1_init_once(void)
{
    uint32_t spins, v, div16, ibrd, rem, fbrd;

    if (uart1_ready)
        return;

    v = mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET);
    if (v & RESET_UART1) {
        mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET, v & ~RESET_UART1);
        for (spins = 0; spins < 1000000u; spins++)
            if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) &
                RESET_UART1)
                break;
    }

    for (int pin = UART1_TX_PIN; pin <= UART1_RX_PIN; pin++) {
        uint32_t pad = mmio_read32(PADS_BANK0_GPIO(pin));

        pad &= ~(PAD_OD | PAD_PUE | PAD_PDE | PAD_ISO);
        pad |= PAD_IE;
        mmio_write32(PADS_BANK0_GPIO(pin), pad);
        mmio_write32(IO_BANK0_GPIO_CTRL(pin), FUNCSEL_UART);
    }

    /* clk_peri is clk_sys after clocks_init(): 125 MHz.  PL011 baud =
     * clk / (16 * (IBRD + FBRD/64)), the same arithmetic uart_dbg.c uses. */
    div16 = 16u * 115200u;
    ibrd = RP2040_SYS_CLK_HZ / div16;
    rem = RP2040_SYS_CLK_HZ - ibrd * div16;
    fbrd = (rem * 64u + div16 / 2u) / div16;
    if (ibrd == 0)
        ibrd = 1;

    mmio_write32(UART1_CR, 0);
    mmio_write32(UART1_ICR, 0x7ffu);
    mmio_write32(UART1_IBRD, ibrd);
    mmio_write32(UART1_FBRD, fbrd & 63u);
    mmio_write32(UART1_LCR_H, (1u << 4) | (3u << 5));  /* FIFO, 8N1 */
    mmio_write32(UART1_CR, (1u << 0) | (1u << 8) | (1u << 9));
    uart1_ready = 1;
}

int plat_uart_count(void)
{
    return 1;
}

int plat_uart_write(int port, const void *buf, int len)
{
    const uint8_t *p = (const uint8_t *)buf;

    if (port != 0 || len < 0 || (!buf && len > 0))
        return -1;

    spin_lock(&uart1_lock);
    uart1_init_once();
    for (int i = 0; i < len; i++) {
        while (mmio_read32(UART1_FR) & UART_FR_TXFF)
            ;
        mmio_write32(UART1_DR, p[i]);
    }
    spin_unlock(&uart1_lock);
    return len;
}

int plat_uart_read(int port, void *buf, int len)
{
    uint8_t *p = (uint8_t *)buf;
    int n = 0;

    if (port != 0 || len < 0 || (!buf && len > 0))
        return -1;

    spin_lock(&uart1_lock);
    uart1_init_once();
    while (n < len && !(mmio_read32(UART1_FR) & UART_FR_RXFE)) {
        p[n++] = (uint8_t)(mmio_read32(UART1_DR) & 0xFFu);
    }
    spin_unlock(&uart1_lock);
    return n;
}

#endif /* TOS_UART1 */
