/* Polled UART0 for RP2040 bring-up, on GPIO0 (TX) and GPIO1 (RX).
 *
 * Deliberately tiny and dependency-free: it is called from the USB clock
 * init, from the bus-reset path and from the bootloader-recovery fallback,
 * before or while the rest of the kernel may be wedged. */

#include "uart_dbg.h"

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
#define CLOCKS_CLK_SYS_CTRL  (*(volatile uint32_t *)(CLOCKS_BASE + 0x3cu))
#define CLOCKS_CLK_SYS_DIV   (*(volatile uint32_t *)(CLOCKS_BASE + 0x40u))
#define CLOCKS_CLK_SYS_SELECTED (*(volatile uint32_t *)(CLOCKS_BASE + 0x44u))

#define IO_BANK0_BASE   0x40014000u
#define PADS_BANK0_BASE 0x4001c000u
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
        for (uint32_t i = 0; i < 1000000u; i++)
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

void dbg_uart_hex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";

    dbg_uart_puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4)
        dbg_uart_putc(hex[(v >> shift) & 0xfu]);
}
