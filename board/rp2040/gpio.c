#include "platform.h"
#include "board.h"
#include "arm_memmap.h"

/* GPIO and ADC for the RP2040.
 *
 * The RP2040 has a pin block with two halves, and both have to be told what
 * to do:
 *
 *   IO_BANK0   FUNCSEL: which peripheral owns the pin (5 = SIO, the
 *              software I/O block where GPIO_OUT/GPIO_IN live)
 *   PADS_BANK0 input enable, output disable, pull-up / pull-down
 *
 * SIO is the reason a pin operation is a single write: GPIO_OUT_SET and
 * GPIO_OUT_CLR are write-1-to-* registers, so setting one pin never needs a
 * read-modify-write of the others (and never races with a second core).
 *
 * The ADC is a 12-bit SAR with five channels: GPIO26..29 are ADC0..3 and
 * channel 4 is the on-chip temperature sensor.  Conversion is started by
 * writing ADC_CS (EN | AINSEL | START_ONCE) and the result is ready when
 * the READY bit drops; the result is in ADC_RESULT.  These registers are
 * taken from the RP2040 datasheet, section 4.9.  Like the rest of this
 * board port they are written from the datasheet, not probed on a board. */

/* SIO: software I/O block. */
#define SIO_GPIO_IN      (SIO_BASE + 0x004u)
#define SIO_GPIO_OUT     (SIO_BASE + 0x010u)
#define SIO_GPIO_OUT_SET (SIO_BASE + 0x014u)
#define SIO_GPIO_OUT_CLR (SIO_BASE + 0x018u)
#define SIO_GPIO_OE      (SIO_BASE + 0x020u)
#define SIO_GPIO_OE_SET  (SIO_BASE + 0x024u)
#define SIO_GPIO_OE_CLR  (SIO_BASE + 0x028u)

#define IO_BANK0_GPIO_CTRL(n) (IO_BANK0_BASE + 0x04u + 8u * (uint32_t)(n))
#define PADS_BANK0_GPIO(n)    (PADS_BANK0_BASE + 0x04u + 4u * (uint32_t)(n))

#define PAD_OD   (1u << 7)   /* output disable (1 = pin is an input only) */
#define PAD_IE   (1u << 6)   /* input enable */
#define PAD_PUE  (1u << 3)   /* pull-up enable  */
#define PAD_PDE  (1u << 2)   /* pull-down enable */
#define FUNCSEL_SIO 5u

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

    /* FUNCSEL 5 hands the pin to SIO; the pad keeps its reset defaults for
     * everything we do not touch (slew rate, drive strength). */
    mmio_write32(IO_BANK0_GPIO_CTRL(pin), FUNCSEL_SIO);

    pad = mmio_read32(PADS_BANK0_GPIO(pin));
    pad &= ~(PAD_OD | PAD_IE | PAD_PUE | PAD_PDE);
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
        /* No PAD_OD: the pin drives.  IE stays on so a read reports the
         * level actually on the pad, not just what we last wrote. */
        mmio_write32(SIO_GPIO_OE_SET, 1u << pin);
        break;
    default:
        return -1;
    }

    mmio_write32(PADS_BANK0_GPIO(pin), pad);
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

#define ADC_BASE       0x4004c000u
#define ADC_CS         (ADC_BASE + 0x00u)
#define ADC_RESULT     (ADC_BASE + 0x04u)

#define ADC_CS_EN         (1u << 0)
#define ADC_CS_START_ONCE (1u << 2)
#define ADC_CS_READY      (1u << 8)
#define ADC_CS_AINSEL_S   12
#define ADC_CS_AINSEL_M   (7u << 12)

/* ADC0..3 are GPIO26..29; channel 4 is the temperature sensor. */
#define RP_ADC_CHANNELS 5

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
    /* The Pico's ADC_VREF comes from a 3.3 V rail through a filter. */
    return 3300;
}

int plat_adc_read(int chan)
{
    uint32_t cs = 0;
    uint32_t spins;

    if (chan < 0 || chan >= RP_ADC_CHANNELS)
        return -1;

    /* Enable the block (idempotent) and start one conversion.  START_ONCE
     * clears READY; the converter sets it again when the sample is in
     * ADC_RESULT.  So READY going high is the "done" edge - polling it the
     * other way round would return the previous channel's sample. */
    mmio_write32(ADC_CS, ADC_CS_EN |
                         ((uint32_t)chan << ADC_CS_AINSEL_S) |
                         ADC_CS_START_ONCE);

    for (spins = 0; spins < 100000u; spins++) {
        cs = mmio_read32(ADC_CS);
        if (cs & ADC_CS_READY)
            break;
    }
    if (!(cs & ADC_CS_READY))
        return -1;              /* stalled block: -1, never a hang */

    /* Take the channel the sample actually came from: RROBIN could change
     * AINSEL behind our back if somebody ever enables it. */
    if (((cs >> ADC_CS_AINSEL_S) & 7u) != (uint32_t)chan)
        return -1;

    return (int)(mmio_read32(ADC_RESULT) & 0xFFFu);
}
