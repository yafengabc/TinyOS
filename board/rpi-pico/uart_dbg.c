/* Polled UART0 for RP2040 bring-up, on GPIO0 (TX) and GPIO1 (RX).
 *
 * Deliberately tiny and dependency-free: it is called from the USB clock
 * init, from the bus-reset path and from the bootloader-recovery fallback,
 * before or while the rest of the kernel may be wedged. */

#include "uart_dbg.h"
#include "platform.h"

#define UART0_BASE   0x40034000u
#define UART_DR      (*(volatile uint32_t *)(UART0_BASE + 0x00u))
#define UART_FR      (*(volatile uint32_t *)(UART0_BASE + 0x18u))
#define UART_IBRD    (*(volatile uint32_t *)(UART0_BASE + 0x24u))
#define UART_FBRD    (*(volatile uint32_t *)(UART0_BASE + 0x28u))
#define UART_LCR_H   (*(volatile uint32_t *)(UART0_BASE + 0x2cu))
#define UART_CR      (*(volatile uint32_t *)(UART0_BASE + 0x30u))
#define UART_ICR     (*(volatile uint32_t *)(UART0_BASE + 0x44u))

#define UART_FR_TXFF 0x00000020u

#define RESETS_BASE     0x4000c000u
#define RESETS_RESET    (*(volatile uint32_t *)(RESETS_BASE + 0x00u))
#define RESETS_DONE     (*(volatile uint32_t *)(RESETS_BASE + 0x08u))
/* pico-sdk rp2040 resets.h: IO_BANK0 = bit 5 (bit 6 is IO_QSPI),
 * PADS_BANK0 = bit 8, UART0 = bit 22.  Releasing bit 6 instead of bit 5
 * leaves the pin-mux registers held in reset, so GPIO0 never becomes UART0
 * TX and the console looks completely dead. */
#define RST_IO_BANK0    (1u << 5)
#define RST_PADS_BANK0  (1u << 8)
#define RST_UART0       (1u << 22)

#define CLOCKS_BASE     0x40008000u
#define CLK_PERI_CTRL   (*(volatile uint32_t *)(CLOCKS_BASE + 0x48u))
#define CLK_PERI_DIV    (*(volatile uint32_t *)(CLOCKS_BASE + 0x4cu))
#define CLOCKS_CLK_SYS_CTRL  (*(volatile uint32_t *)(CLOCKS_BASE + 0x3cu))
#define CLOCKS_CLK_SYS_DIV   (*(volatile uint32_t *)(CLOCKS_BASE + 0x40u))
#define CLOCKS_CLK_SYS_SELECTED (*(volatile uint32_t *)(CLOCKS_BASE + 0x44u))

#define IO_BANK0_BASE   0x40014000u
#define PADS_BANK0_BASE 0x4001c000u
#define SIO_BASE        0xd0000000u
#define GPIO_CTRL(n) (*(volatile uint32_t *)(IO_BANK0_BASE + 0x04u + 8u * (n)))
#define GPIO_PAD(n)  (*(volatile uint32_t *)(PADS_BANK0_BASE + 0x04u + 4u * (n)))

void dbg_uart_init(uint32_t clk_hz)
{
    /* The ROM bootloader only releases the blocks it uses (XIP, USB).  The
     * GPIO banks and UART0 are still held in reset when control reaches the
     * kernel, and writes to their registers are ignored until they are
     * released.  This was why both the GP0 beacon and the UART looked dead
     * while USB worked. */
    {
        uint32_t want = RST_IO_BANK0 | RST_PADS_BANK0 | RST_UART0;
        uint32_t m = RESETS_RESET & ~want;

        RESETS_RESET = m;
        for (uint32_t i = 0; i < plat_pre_tick_spins(1000000u); i++)
            if ((RESETS_DONE & want) == want)
                break;
    }

    /* clk_peri feeds UART0.  The reset/boot-ROM state does not guarantee it
     * is enabled, and the RP2040 port has no other peripheral clock setup;
     * without this the UART registers program fine and the pin never moves.
     * AUXSRC 0 = clk_sys. */
    CLK_PERI_CTRL = (1u << 11);

    for (int n = 0; n <= 1; n++) {
        /* ISO off, input enable on, output enable on, function 2 = UART0. */
        GPIO_PAD(n)  = (GPIO_PAD(n) & ~(1u << 8)) | (1u << 6);
        GPIO_PAD(n) &= ~(1u << 7);
        GPIO_CTRL(n) = (GPIO_CTRL(n) & ~0x1fu) | 2u;
    }

    UART_CR = 0;
    UART_ICR = 0x7ffu;

    dbg_uart_set_peri_hz(clk_hz);

    UART_LCR_H = (1u << 4) | (3u << 5);     /* FIFO enable, 8N1            */
    UART_CR = (1u << 0) | (1u << 8) | (1u << 9); /* UARTEN | TXE | RXE     */
}

void dbg_uart_set_peri_hz(uint32_t clk_hz)
{
    /* PL011: baud = clk_peri / (16 * (IBRD + FBRD/64)).  clk_peri can be
     * switched between clk_sys (12 MHz) and PLL_USB (48 MHz) at runtime to
     * A/B whether the USB register interface needs a faster clock; the
     * divisor has to follow or the console turns into garbage. */
    uint32_t div16 = 16u * 115200u;
    uint32_t ibrd = clk_hz / div16;
    uint32_t rem  = clk_hz - ibrd * div16;
    uint32_t fbrd = (rem * 64u + div16 / 2u) / div16;

    if (ibrd == 0)
        ibrd = 1;
    UART_IBRD = ibrd;
    UART_FBRD = fbrd & 63u;
    /* PL011 latches the divisor pair into the baud generator on an LCR_H
     * write.  Re-assert the same 8N1 + FIFO value so a mid-run clock switch
     * is not left using the previous divisor. */
    UART_LCR_H = (1u << 4) | (3u << 5);
}

void dbg_uart_putc(char c)
{
    while (UART_FR & UART_FR_TXFF)
        ;
    UART_DR = (uint32_t)(uint8_t)c;
}

void dbg_uart_puts(const char *s)
{
    while (s && *s) {
        if (*s == '\n')
            dbg_uart_putc('\r');
        dbg_uart_putc(*s++);
    }
}

void dbg_uart_hex(uint32_t v){
    static const char hex[] = "0123456789abcdef";

    dbg_uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        dbg_uart_putc(hex[(v >> shift) & 0xfu]);
}

/* Console UART programming for /proc/uart0: CR, FR, IBRD, FBRD, LCR_H,
 * GPIO0_CTRL, PAD0, GPIO1_CTRL, PAD1, CLK_PERI_CTRL, CLK_PERI_DIV,
 * RESETS_DONE.  Read-only, so it is safe on a boot whose console works -
 * the point is to be able to compare a working boot against one that has
 * lost its transmitter. */
void plat_uart_debug(uint32_t *out)
{
    out[0]  = UART_CR;
    out[1]  = UART_FR;
    out[2]  = UART_IBRD;
    out[3]  = UART_FBRD;
    out[4]  = UART_LCR_H;
    out[5]  = GPIO_CTRL(0);
    out[6]  = GPIO_PAD(0);
    out[7]  = GPIO_CTRL(1);
    out[8]  = GPIO_PAD(1);
    out[9]  = CLK_PERI_CTRL;
    out[10] = CLK_PERI_DIV;
    out[11] = RESETS_DONE;
}

/* Transmit 0x55 and watch GP0 through SIO_GPIO_IN for whole frames, with
 * GP25 (the LED) as the control pin - see kernel/platform.h.  Sampling only
 * right after the write lands inside the start bit and reads "always low"
 * on a healthy pin. */
int plat_uart_tx_probe(uint32_t *out)
{
    uint32_t lo = 0, hi = 0, trans = 0, prev = 2u, spins;
    uint32_t idle_lo = 0, idle_hi = 0, gp1_hi = 0, led_hi = 0;

    for (int s = 0; s < 2048; s++) {
        uint32_t v = *(volatile uint32_t *)(SIO_BASE + 0x004u);

        if (v & 1u)
            idle_hi++;
        else
            idle_lo++;
    }

    for (int n = 0; n < 32; n++) {
        for (spins = 0; spins < 200000u; spins++)
            if (!(UART_FR & UART_FR_TXFF))
                break;
        if (spins == 200000u)
            return 0;
        UART_DR = 0x55u;
        for (int s = 0; s < 2048; s++) {
            uint32_t v = *(volatile uint32_t *)(SIO_BASE + 0x004u);
            uint32_t pin = v & 1u;

            if (v & (1u << 25))
                led_hi++;
            if (v & 2u)
                gp1_hi++;
            if (pin)
                hi++;
            else
                lo++;
            if (prev != 2u && pin != prev)
                trans++;
            prev = pin;
        }
    }
    for (spins = 0; spins < 2000000u; spins++)
        if (!(UART_FR & (1u << 3)))          /* BUSY */
            break;
    out[0] = lo;
    out[1] = hi;
    out[2] = trans;
    out[3] = UART_FR;
    out[4] = idle_lo;
    out[5] = idle_hi;
    out[6] = gp1_hi;
    out[7] = led_hi;
    return 1;
}

/* Drive the TX pin from SIO and read it back: see the RP2350 port for why
 * this is the question that separates "the pad cannot pull the line high"
 * (hardware, no receiver will ever work) from "the UART's output is not
 * reaching the pad". */
int plat_gpio_drive_probe(uint32_t *out)
{
    uint32_t ctrl = *(volatile uint32_t *)(IO_BANK0_BASE + 0x04u);
    uint32_t hi1 = 0, lo1 = 0, hi0 = 0, lo0 = 0;
    volatile uint32_t *sio_in   = (volatile uint32_t *)(SIO_BASE + 0x004u);
    volatile uint32_t *sio_out_s= (volatile uint32_t *)(SIO_BASE + 0x014u);
    volatile uint32_t *sio_out_c= (volatile uint32_t *)(SIO_BASE + 0x018u);
    volatile uint32_t *sio_oe_s = (volatile uint32_t *)(SIO_BASE + 0x024u);
    volatile uint32_t *sio_oe_c = (volatile uint32_t *)(SIO_BASE + 0x028u);

    *(volatile uint32_t *)(IO_BANK0_BASE + 0x04u) = 5u;   /* FUNCSEL_SIO */
    *sio_oe_s = 1u;

    *sio_out_s = 1u;
    out[4] = *(volatile uint32_t *)(IO_BANK0_BASE + 0x04u);  /* must read 5 */
    out[5] = *(volatile uint32_t *)(SIO_BASE + 0x020u);      /* must have bit 0 */
    for (int s = 0; s < 2048; s++) {
        if (*sio_in & 1u)
            hi1++;
        else
            lo1++;
    }
    *sio_out_c = 1u;
    for (int s = 0; s < 2048; s++) {
        if (*sio_in & 1u)
            hi0++;
        else
            lo0++;
    }

    *sio_oe_c = 1u;
    *(volatile uint32_t *)(IO_BANK0_BASE + 0x04u) = ctrl;
    out[0] = hi1;
    out[1] = lo1;
    out[2] = hi0;
    out[3] = lo0;
    return 1;
}
