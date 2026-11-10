/* Minimal RP2040 blinker: GP0 and GP25 (onboard LED), no clocks, no UART,
 * no USB.  Used only to prove that a hand-built UF2 executes on the board
 * and that the GPIO block is out of reset before the kernel runs.
 *
 * Delay is a plain busy loop, so the blink rate follows clk_sys: the boot
 * ROM leaves it at 12 MHz, while TinyOS raises it to 125 MHz.  500k
 * iterations is ~0.25 s at 12 MHz and ~25 ms at 125 MHz - both obviously
 * visible on a scope.  Bigger numbers are a trap: the earlier 50M version
 * had a 25-second half period at 12 MHz and looked dead. */

#define R(a) (*(volatile unsigned *)(a))

#define PADS_BANK0   0x4001c000u
#define IO_BANK0     0x40014000u
#define SIO          0xd0000000u
#define RESETS       0x4000c000u

static void gpio_bank_unreset(void)
{
    unsigned want = (1u << 6) | (1u << 8);      /* IO_BANK0 | PADS_BANK0 */
    R(RESETS + 0x00u) &= ~want;
    for (volatile int i = 0; i < 1000000; i++)
        if ((R(RESETS + 0x08u) & want) == want)
            break;
}

static void pin_sio(int n)
{
    /* PADS_BANK0 + 4*n is VOLTAGE_SELECT for n = 0; the per-GPIO pad
     * registers start at +0x04. */
    R(PADS_BANK0 + 0x04u + 4u * n) =
        (R(PADS_BANK0 + 0x04u + 4u * n) | (1u << 6))    /* IE             */
        & ~(1u << 7) & ~(1u << 8);                      /* OD off, ISO on */
    R(IO_BANK0 + 0x04u + 8u * n) = 5u;                  /* F5 = SIO       */
}

int main(void)
{
    gpio_bank_unreset();
    pin_sio(0);
    pin_sio(25);
    R(SIO + 0x024u) = (1u << 0) | (1u << 25);   /* OE_SET */

    for (;;) {
        R(SIO + 0x014u) = (1u << 0) | (1u << 25);   /* high */
        for (volatile int i = 0; i < 500000; i++)
            ;
        R(SIO + 0x018u) = (1u << 0) | (1u << 25);   /* low  */
        for (volatile int i = 0; i < 500000; i++)
            ;
    }
}
