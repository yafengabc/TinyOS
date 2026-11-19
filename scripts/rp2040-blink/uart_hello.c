/* Minimal RP2040 UART0 probe for bring-up.
 *
 * Same boot2 / start.S / link.ld as blink.c.  It does three things at once:
 *   1. waves GP25 (the Pico LED) so liveness is visible without any UART;
 *   2. prints a tagged line at three candidate clk_peri rates - whichever
 *      tag arrives intact on the host is the real clock;
 *   3. watches UART0 RX for Ctrl-D and calls the ROM's reset_usb_boot(), so
 *      the board mounts BOOTSEL again without a human pressing BOOT + RESET.
 *
 * Bit definitions checked against pico-sdk hardware_regs:
 *   RESETS_RESET_UART0     = bit 22
 *   RESETS_RESET_IO_BANK0  = bit 5   (bit 6 is IO_QSPI - a previous version
 *                                     used 6 and the pin mux never worked)
 *   RESETS_RESET_PADS_BANK0= bit 8
 *   CLK_PERI_CTRL.ENABLE   = bit 11, AUXSRC = 0 (clk_sys)
 * Dividers are precomputed because Cortex-M0+ has no hardware divide and
 * this image links with -nostdlib. */

#define R(a) (*(volatile unsigned *)(a))

#define RESETS   0x4000c000u
#define PADS     0x4001c000u
#define IO       0x40014000u
#define CLOCKS   0x40008000u
#define UART0    0x40034000u
#define SIO      0xd0000000u

#define UART_DR   R(UART0 + 0x00u)
#define UART_FR   R(UART0 + 0x18u)
#define UART_IBRD R(UART0 + 0x24u)
#define UART_FBRD R(UART0 + 0x28u)
#define UART_LCRH R(UART0 + 0x2cu)
#define UART_CR   R(UART0 + 0x30u)
#define UART_ICR  R(UART0 + 0x44u)

#define FR_TXFF (1u << 5)
#define FR_RXFE (1u << 4)
#define FR_BUSY (1u << 3)

/* ROM function table (RP2040 flavour: lookup(table, code)). */
typedef void *(*rom_lookup_t)(unsigned short *table, unsigned code);
typedef void  (*rom_usb_boot_t)(unsigned gpio_mask, unsigned disable_mask);
#define ROM_FUNC_RESET_USB_BOOT 0x4255u   /* ROM_TABLE_CODE('U','B') */

static void enter_bootsel(void)
{
    rom_lookup_t lookup = (rom_lookup_t)(unsigned)(*(volatile unsigned short *)0x18u);
    unsigned short *functab =
        (unsigned short *)(unsigned)(*(volatile unsigned short *)0x14u);
    rom_usb_boot_t boot;

    if (!lookup || !functab)
        return;
    boot = (rom_usb_boot_t)((unsigned)lookup(functab, ROM_FUNC_RESET_USB_BOOT) | 1u);
    if (!boot)
        return;
    boot(0u, 0u);                 /* no activity LED, keep both interfaces */
    for (;;)
        ;
}

static void putc_(char c)
{
    while (UART_FR & FR_TXFF)
        ;
    UART_DR = (unsigned char)c;
}

static void puts_(const char *s)
{
    while (*s) {
        if (*s == '\n')
            putc_('\r');
        putc_(*s++);
    }
    while (UART_FR & FR_BUSY)
        ;
}

static void uart_div(unsigned ibrd, unsigned fbrd)
{
    UART_CR = 0;                  /* disable while reprogramming */
    UART_ICR = 0x7ffu;
    UART_IBRD = ibrd;
    UART_FBRD = fbrd;
    UART_LCRH = (1u << 4) | (3u << 5);       /* FIFO, 8N1 */
    UART_CR = 1u | (1u << 8) | (1u << 9);   /* EN | TXE | RXE */
}

/* clk / (16 * 115200) = 6.5104 / 26.0417 / 67.8168 */
static const struct probe {
    unsigned ibrd, fbrd;
    const char *msg;
} cases[] = {
    {  6, 33, "\r\n[uart 12MHz]\r\n"  },
    { 26,  3, "\r\n[uart 48MHz]\r\n"  },
    { 67, 52, "\r\n[uart 125MHz]\r\n" },
};

static void check_rx(void)
{
    /* Non-blocking: if a Ctrl-D landed at this baud rate, go to BOOTSEL. */
    if (!(UART_FR & FR_RXFE) && (UART_DR & 0xffu) == 0x04u)
        enter_bootsel();
}

int main(void)
{
    unsigned want = (1u << 5) | (1u << 8) | (1u << 22); /* IO_BANK0|PADS|UART0 */

    R(RESETS + 0x00u) &= ~want;
    for (volatile int i = 0; i < 1000000; i++)
        if ((R(RESETS + 0x08u) & want) == want)
            break;

    R(CLOCKS + 0x48u) = (1u << 11);        /* clk_peri ENABLE, AUXSRC=clk_sys */

    for (int n = 0; n <= 1; n++) {
        unsigned p = PADS + 0x04u + 4u * n;
        R(p) = (R(p) | (1u << 6)) & ~(1u << 7) & ~(1u << 8);
        R(IO + 0x04u + 8u * n) = 2u;       /* F2 = UART0 */
    }

    /* GP25 = SIO, output.  Blinking proves boot2 + vector table + RESETS. */
    {
        unsigned p = PADS + 0x04u + 4u * 25u;
        R(p) = (R(p) | (1u << 6)) & ~(1u << 7) & ~(1u << 8);
        R(IO + 0x04u + 8u * 25u) = 5u;
        R(SIO + 0x024u) = (1u << 25);      /* GPIO_OE_SET */
    }

    for (unsigned loop = 0;; loop++) {
        R(SIO + (loop & 1u ? 0x018u : 0x014u)) = (1u << 25); /* CLR / SET */
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            uart_div(cases[i].ibrd, cases[i].fbrd);
            puts_(cases[i].msg);
            /* Stay at this baud long enough for a host-side Ctrl-D burst. */
            for (volatile int j = 0; j < 120000; j++)
                check_rx();
        }
    }
}
