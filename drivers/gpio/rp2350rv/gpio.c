#include "platform.h"
#include "board.h"
#include "rp2350rv_memmap.h"

/* GPIO and ADC for the RP2350.
 *
 * The pad and IO_BANK0 register layouts survived the RP2040 -> RP2350
 * move, but their base addresses did not, and the SIO GPIO registers are
 * now 8 bytes apart (64-bit words, because the RP2350B has 48 pins).
 * All of the offsets below come from the RP2350 register headers.
 *
 * Pico 2 (RP2350A) exposes GPIO0..29, so 30 pins, same as the Pico.
 * FUNCSEL 5 hands a pin to SIO; FUNCSEL 2 is UART0 TX/RX. */

#define PAD_OD   (1u << 7)
#define PAD_IE   (1u << 6)
/* RP2350-only isolation bit: reset value is 1, so every configured pin
 * must explicitly clear it or it stays electrically disconnected. */
#define PAD_ISO  (1u << 8)
#define PAD_PUE  (1u << 3)
#define PAD_PDE  (1u << 2)

#define RP_GPIO_PINS 30

int plat_gpio_count(void)
{
    return RP_GPIO_PINS;
}

int plat_gpio_configure(int pin, int mode)
{
    uint32_t pad;

    if (pin < 0 || pin >= RP_GPIO_PINS)
        return -1;

    mmio_write32(RP_GPIO_CTRL(pin), RP_GPIO_FUNC_SIO);

    pad = mmio_read32(RP_GPIO_PAD(pin));
    pad &= ~(PAD_OD | PAD_IE | PAD_PUE | PAD_PDE | PAD_ISO);
    pad |= PAD_IE;

    switch (mode) {
    case PLAT_GPIO_IN:
        pad |= PAD_OD;
        mmio_write32(SIO_GPIO_OE_CLR, 1u << pin);
        break;
    case PLAT_GPIO_IN_PULLUP:
        pad |= PAD_OD | PAD_PUE;
        mmio_write32(SIO_GPIO_OE_CLR, 1u << pin);
        break;
    case PLAT_GPIO_IN_PULLDOWN:
        pad |= PAD_OD | PAD_PDE;
        mmio_write32(SIO_GPIO_OE_CLR, 1u << pin);
        break;
    case PLAT_GPIO_OUT:
        mmio_write32(SIO_GPIO_OE_SET, 1u << pin);
        break;
    default:
        return -1;
    }

    mmio_write32(RP_GPIO_PAD(pin), pad);
    return 0;
}

int plat_gpio_write(int pin, int value)
{
    if (pin < 0 || pin >= RP_GPIO_PINS)
        return -1;
    mmio_write32(value ? SIO_GPIO_OUT_SET : SIO_GPIO_OUT_CLR, 1u << pin);
    return 0;
}

int plat_gpio_read(int pin)
{
    if (pin < 0 || pin >= RP_GPIO_PINS)
        return -1;
    return (mmio_read32(SIO_GPIO_IN) >> pin) & 1u;
}

/* ---- ADC ------------------------------------------------------------- */

#define ADC_CS         (ADC_BASE + 0x00u)
#define ADC_RESULT     (ADC_BASE + 0x04u)

#define ADC_CS_EN         (1u << 0)
#define ADC_CS_TS_EN      (1u << 1)   /* temperature sensor, channel 4 only */
#define ADC_CS_START_ONCE (1u << 2)
#define ADC_CS_READY      (1u << 8)
#define ADC_CS_ERR        (1u << 9)
#define ADC_CS_AINSEL_S   12

/* ADC0..3 are GPIO26..29; channel 4 is the temperature sensor. */
#define RP_ADC_CHANNELS 5          /* GPIO26..29 plus the temperature sensor */
#define ADC_TEMP_CHAN   4
#define ADC_INPUT_PINS  4          /* channels 0..3 are GPIO26..29 on a 30-pin part */

/* Bring the block out of reset and clock it, once.
 *
 * Nothing used to do this: RESET_ADC stays asserted after boot (so the block
 * is held in reset) and CLK_ADC is off, which is why every conversion timed
 * out with "conversion failed" - READY never came up.  The RP2040 port got
 * away with it because its ROM leaves the ADC clocked; the RP2350 boot path
 * we use never touches it.
 *
 * CLK_ADC has no clk_sys source on this chip - the choices are pll_usb,
 * pll_sys, rosc, xosc and the two GPINs - so it runs from PLL_SYS with an
 * integer divider of 4: 150 / 4 = 37.5 MHz, under the 48 MHz ceiling.  If
 * the PLL never locked the board runs at ~12 MHz and divider 4 is still in
 * range, so the same setting covers the fallback boot. */
static int adc_up;

static void adc_init_hw(void)
{
    if (adc_up)
        return;

    mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET,
                 mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET) & ~RESET_ADC);
    for (uint32_t i = 0; i < 1000u; i++) {
        if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) & RESET_ADC)
            break;
    }

    mmio_write32(CLOCKS_BASE + CLK_ADC_DIV_OFFSET, 4u << 16);
    mmio_write32(CLOCKS_BASE + CLK_ADC_CTRL_OFFSET,
                 CLK_ADC_CTRL_ENABLE |
                 (CLK_ADC_CTRL_AUXSRC_PLL_SYS << 5));
    for (uint32_t i = 0; i < 1000u; i++) {
        if (mmio_read32(CLOCKS_BASE + CLK_ADC_SELECTED_OFFSET) & 1u)
            break;
    }

    /* Analog inputs need the pad out of isolation and its digital input
     * off, or the signal never reaches the converter (RP2350 pads reset
     * with ISO set).  Only channels 0..3 have a pin; channel 4 is the
     * on-die temperature sensor.  A pin that was digital and is now read as
     * an analog input stays in this state - call plat_gpio_configure() to
     * hand it back to SIO. */
    for (int ch = 0; ch < ADC_INPUT_PINS; ch++) {
        int pin = 26 + ch;
        mmio_write32(RP_GPIO_CTRL(pin), RP_GPIO_FUNC_NULL);
        mmio_write32(RP_GPIO_PAD(pin),
                     mmio_read32(RP_GPIO_PAD(pin)) &
                     ~(PAD_ISO | PAD_IE | PAD_PUE | PAD_PDE | PAD_OD));
    }

    adc_up = 1;
}

int plat_adc_count(void)
{
    return RP_ADC_CHANNELS;
}

int plat_adc_bits(void)
{
    return 12;
}

int plat_adc_vref_mv(void)
{
    return 3300;
}

/* One conversion.  Returns 0 and stores the 12-bit result, or -1 on error. */
static int adc_convert(int chan, uint32_t *out)
{
    uint32_t cs = 0;

    /* Channel 4 is the on-die sensor, not a pin: it only converts when
     * TS_EN is set, and without it a temperature read returns 0 mV. */
    mmio_write32(ADC_CS, ADC_CS_EN |
                         ((uint32_t)chan << ADC_CS_AINSEL_S) |
                         (chan == ADC_TEMP_CHAN ? ADC_CS_TS_EN : 0u) |
                         ADC_CS_START_ONCE);

    for (uint32_t spins = 0; spins < 100000u; spins++) {
        cs = mmio_read32(ADC_CS);
        if (cs & (ADC_CS_READY | ADC_CS_ERR))
            break;
    }
    if (cs & ADC_CS_ERR) {
        /* Sticky, write-one-to-clear: leaving it set would fail every later
         * conversion for a reason that is already over. */
        mmio_write32(ADC_CS, ADC_CS_ERR);
        return -1;
    }
    if (!(cs & ADC_CS_READY))
        return -1;
    if (((cs >> ADC_CS_AINSEL_S) & 7u) != (uint32_t)chan)
        return -1;

    *out = mmio_read32(ADC_RESULT) & 0xFFFu;
    return 0;
}

int plat_adc_read(int chan)
{
    uint32_t v = 0;

    if (chan < 0 || chan >= RP_ADC_CHANNELS)
        return -1;

    adc_init_hw();

    /* The sensor's first conversion after TS_EN comes back as 0: it needs a
     * conversion to settle.  Throw one away so the first `adc 4` after boot
     * reads a real temperature instead of 0 mV. */
    if (chan == ADC_TEMP_CHAN)
        (void)adc_convert(chan, &v);

    if (adc_convert(chan, &v) < 0)
        return -1;
    return (int)v;
}
