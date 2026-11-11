#include "platform.h"
#include "board.h"
#include "arm_memmap.h"
#include "cpu.h"

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
 * writing ADC_CS (EN | AINSEL | START_ONCE); START_ONCE clears READY and the
 * converter sets it again when the sample is in ADC_RESULT.  These registers
 * are taken from the RP2040 datasheet, section 4.9.  Like the rest of this
 * board port they are written from the datasheet, not probed on a board.
 *
 * The block is out of reset after the boot ROM, but its clock (clk_adc) is
 * gated.  Without that clock a conversion never completes and READY never
 * sets, which is exactly the "conversion failed" every channel reported.
 * adc_init_once() below enables clk_adc (PLL_USB 48 MHz, or the 12 MHz
 * crystal if USB never came up), releases the reset bit defensively, and
 * detaches GPIO26..29 from the digital pin mux before the first sample. */

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

#define ADC_CS_EN          (1u << 0)
#define ADC_CS_TS_EN       (1u << 1)
#define ADC_CS_START_ONCE  (1u << 2)
#define ADC_CS_READY       (1u << 8)
#define ADC_CS_ERR         (1u << 9)
#define ADC_CS_ERR_STICKY  (1u << 10)
#define ADC_CS_AINSEL_S    12
#define ADC_CS_AINSEL_M    (7u << 12)

#define CLK_ADC_CTRL_OFFSET 0x60u
#define CLK_ADC_DIV_OFFSET  0x64u
#define CLK_ADC_ENABLE      (1u << 11)
#define CLK_ADC_AUX_PLL_USB 0x0u
#define CLK_ADC_AUX_XOSC    0x3u

#define PLL_USB_CS          (PLL_USB_BASE + 0x00u)

#define PAD_ISO             (1u << 8)   /* pad isolation */
#define FUNCSEL_PWM         4u
#define FUNCSEL_NULL        31u

/* ADC0..3 are GPIO26..29; channel 4 is the temperature sensor. */
#define RP_ADC_CHANNELS 5
#define RP_ADC_FIRST_PIN 26
#define RP_ADC_LAST_PIN  29

static spin_t adc_lock = SPIN_INIT;
static int adc_ready;

static void adc_pin_init(int pin)
{
    uint32_t pad = mmio_read32(PADS_BANK0_GPIO(pin));

    /* FUNCTION_NULL, no digital input buffer, no pulls: the pad is an analog
     * input, and leaving IE or a pull on it would load the source. */
    mmio_write32(IO_BANK0_GPIO_CTRL(pin), FUNCSEL_NULL);
    pad &= ~(PAD_IE | PAD_PUE | PAD_PDE | PAD_ISO);
    mmio_write32(PADS_BANK0_GPIO(pin), pad);
    mmio_write32(SIO_GPIO_OE_CLR, 1u << pin);
}

static void adc_init_once(void)
{
    uint32_t spins;
    uint32_t aux;

    if (adc_ready)
        return;

    /* The boot ROM usually leaves the ADC out of reset, but a warm reset or
     * a boot path that does not run runtime_init can leave bit 0 set.  Clear
     * it and wait for RESET_DONE before touching ADC_CS. */
    spins = mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET);
    if (spins & RESET_ADC) {
        mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET, spins & ~RESET_ADC);
        for (spins = 0; spins < 100000u; spins++)
            if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) &
                RESET_ADC)
                break;
    }

    for (int pin = RP_ADC_FIRST_PIN; pin <= RP_ADC_LAST_PIN; pin++)
        adc_pin_init(pin);

    /* clk_adc has no divider of its own; 48 MHz from PLL_USB is the
     * documented maximum.  If USB never came up, PLL_USB is not locked and
     * the 12 MHz crystal is a safe (slower) fallback - a conversion still
     * completes, which is more useful than reporting every channel dead. */
    aux = (mmio_read32(PLL_USB_CS) & PLL_CS_LOCK) ? CLK_ADC_AUX_PLL_USB
                                                  : CLK_ADC_AUX_XOSC;
    mmio_write32(CLOCKS_BASE + CLK_ADC_DIV_OFFSET, CLK_DIV_1);
    mmio_write32(CLOCKS_BASE + CLK_ADC_CTRL_OFFSET,
                 CLK_ADC_ENABLE | (aux << 5));

    /* Enable the converter and the temperature sensor.  START_ONCE in
     * plat_adc_read() starts each sample; EN has to be in every CS write
     * because CS is one register, not a set of sticky control bits. */
    mmio_write32(ADC_CS, ADC_CS_EN | ADC_CS_TS_EN);

    adc_ready = 1;
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
    /* The Pico's ADC_VREF comes from a 3.3 V rail through a filter. */
    return 3300;
}

int plat_adc_read(int chan)
{
    uint32_t cs = 0;
    uint32_t spins;

    if (chan < 0 || chan >= RP_ADC_CHANNELS)
        return -1;

    adc_init_once();

    /* The ADC is one shared block: two cores (or a task and /dev/adcN) must
     * not interleave AINSEL/START.  The lock is a no-op on a uniprocessor. */
    spin_lock(&adc_lock);

    /* Clear any sticky error from an earlier stalled conversion. */
    mmio_write32(ADC_CS, ADC_CS_EN | ADC_CS_TS_EN | ADC_CS_ERR_STICKY);

    /* START_ONCE clears READY; the converter sets it again when the sample
     * is in ADC_RESULT.  READY high is the done edge. */
    mmio_write32(ADC_CS, ADC_CS_EN |
                         ADC_CS_TS_EN |
                         ((uint32_t)chan << ADC_CS_AINSEL_S) |
                         ADC_CS_START_ONCE);

    for (spins = 0; spins < 100000u; spins++) {
        cs = mmio_read32(ADC_CS);
        if (cs & ADC_CS_READY)
            break;
    }
    if (!(cs & ADC_CS_READY) || (cs & ADC_CS_ERR)) {
        spin_unlock(&adc_lock);
        return -1;              /* stalled or failed: -1, never a hang */
    }

    /* Take the channel the sample actually came from: RROBIN could change
     * AINSEL behind our back if somebody ever enables it. */
    if (((cs >> ADC_CS_AINSEL_S) & 7u) != (uint32_t)chan) {
        spin_unlock(&adc_lock);
        return -1;
    }

    cs = mmio_read32(ADC_RESULT) & 0xFFFu;
    spin_unlock(&adc_lock);
    return (int)cs;
}

#ifdef TOS_PWM

/* ---- PWM --------------------------------------------------------------
 *
 * Eight slices, each with an A and a B output, so 16 channels.  A GPIO is
 * routed to a slice with FUNCSEL 4; the mapping repeats every 16 pins:
 *
 *   slice   = (gpio >> 1) & 7
 *   channel = gpio & 1
 *
 * Both channels of one slice share the divider, TOP and enable bit, while
 * CC.A and CC.B are independent.  The frequency is therefore a property of
 * the slice, not of the pin: configuring GP0 and GP1 at different rates
 * leaves both at the second rate.  That is a hardware fact, not a driver
 * choice, and the pwm(1) command prints the slice so it is visible.
 *
 * clk_sys is 125 MHz.  A period is div * (TOP + 1) cycles, with divider
 * 8.4 fixed point; this driver uses the integer part only.  For the common
 * audio/LED range that is exact enough, and the alternative (a fractional
 * divider) would need a 64-bit multiply user space does not link. */

#define PWM_BASE        0x40050000u
#define PWM_SLICE(s)    (PWM_BASE + 0x14u * (uint32_t)(s))
#define PWM_CSR(s)      (PWM_SLICE(s) + 0x00u)
#define PWM_DIV(s)      (PWM_SLICE(s) + 0x04u)
#define PWM_CTR(s)      (PWM_SLICE(s) + 0x08u)
#define PWM_CC(s)       (PWM_SLICE(s) + 0x0cu)
#define PWM_TOP(s)      (PWM_SLICE(s) + 0x10u)

#define PWM_CSR_EN      (1u << 0)
#define PWM_SLICES      8u

static spin_t pwm_lock = SPIN_INIT;
static uint32_t pwm_active[PWM_SLICES];   /* bit 0 = A, bit 1 = B */
static int pwm_ready;

static void pwm_init_once(void)
{
    uint32_t v, spins;

    if (pwm_ready)
        return;

    v = mmio_read32(RESETS_BASE + RESETS_RESET_OFFSET);
    if (v & RESET_PWM) {
        mmio_write32(RESETS_BASE + RESETS_RESET_OFFSET, v & ~RESET_PWM);
        for (spins = 0; spins < 100000u; spins++)
            if (mmio_read32(RESETS_BASE + RESETS_RESET_DONE_OFFSET) &
                RESET_PWM)
                break;
    }
    pwm_ready = 1;
}

static void pwm_pin_init(int gpio)
{
    uint32_t pad = mmio_read32(PADS_BANK0_GPIO(gpio));

    /* PWM is an output function: drive the pad, keep the input buffer so a
     * read still reports the level, and remove the pulls. */
    pad &= ~(PAD_OD | PAD_PUE | PAD_PDE | PAD_ISO);
    pad |= PAD_IE;
    mmio_write32(PADS_BANK0_GPIO(gpio), pad);
    mmio_write32(IO_BANK0_GPIO_CTRL(gpio), FUNCSEL_PWM);
    mmio_write32(SIO_GPIO_OE_CLR, 1u << gpio);
}

int plat_pwm_count(void)
{
    return (int)(PWM_SLICES * 2u);
}

int plat_pwm_set(int gpio, int freq_hz, int duty_permille)
{
    uint32_t ticks, div, top, cc, slice, chan, old;

    if (gpio < 0 || gpio >= RP_GPIO_PINS || freq_hz <= 0 ||
        duty_permille < 0 || duty_permille > 1000)
        return -1;

    slice = ((uint32_t)gpio >> 1) & (PWM_SLICES - 1u);
    chan  = (uint32_t)gpio & 1u;

    /* Pick the largest TOP that fits 16 bits, then the divider that gets
     * the requested period.  This favours resolution at low frequencies.
     * 255 * 65536 is the slowest period the 8.4 divider and 16-bit TOP can
     * represent; below that, say no instead of silently running 7.4 Hz. */
    if ((uint32_t)freq_hz > RP2040_SYS_CLK_HZ / 2u)
        return -1;
    ticks = RP2040_SYS_CLK_HZ / (uint32_t)freq_hz;
    if (ticks > 255u * 65536u)
        return -1;
    if (ticks < 2u)
        ticks = 2u;
    div = (ticks + 65535u) / 65536u;
    if (div < 1u)
        div = 1u;
    if (div > 255u)
        div = 255u;
    top = ticks / div;
    if (top < 2u)
        top = 2u;
    if (top > 65536u)
        top = 65536u;
    top--;

    /* CC is a 16-bit compare.  duty=1000 wants CC > TOP; when TOP is
     * already 0xffff, TOP itself is the closest representable value. */
    if (duty_permille >= 1000)
        cc = (top < 0xFFFFu) ? top + 1u : top;
    else
        cc = ((top + 1u) * (uint32_t)duty_permille) / 1000u;

    spin_lock(&pwm_lock);
    pwm_init_once();

    mmio_write32(PWM_DIV(slice), div << 4);
    mmio_write32(PWM_TOP(slice), top);

    old = mmio_read32(PWM_CC(slice));
    if (chan == 0)
        old = (old & 0xFFFF0000u) | (cc & 0xFFFFu);
    else
        old = (old & 0x0000FFFFu) | ((cc & 0xFFFFu) << 16);
    mmio_write32(PWM_CC(slice), old);

    mmio_write32(PWM_CTR(slice), 0);
    mmio_write32(PWM_CSR(slice), PWM_CSR_EN);
    pwm_active[slice] |= 1u << chan;
    pwm_pin_init(gpio);
    spin_unlock(&pwm_lock);
    return 0;
}

int plat_pwm_off(int gpio)
{
    uint32_t slice, chan, old;

    if (gpio < 0 || gpio >= RP_GPIO_PINS)
        return -1;

    slice = ((uint32_t)gpio >> 1) & (PWM_SLICES - 1u);
    chan  = (uint32_t)gpio & 1u;

    spin_lock(&pwm_lock);
    pwm_init_once();

    old = mmio_read32(PWM_CC(slice));
    if (chan == 0)
        old &= 0xFFFF0000u;
    else
        old &= 0x0000FFFFu;
    mmio_write32(PWM_CC(slice), old);

    pwm_active[slice] &= ~(1u << chan);
    if (pwm_active[slice] == 0u) {
        mmio_write32(PWM_CSR(slice), 0);
        mmio_write32(PWM_DIV(slice), 1u << 4);
        mmio_write32(PWM_TOP(slice), 0xFFFFu);
    }
    spin_unlock(&pwm_lock);

    /* Give the pin back to SIO and drive it low, so a stopped PWM output
     * does not float. */
    mmio_write32(IO_BANK0_GPIO_CTRL(gpio), FUNCSEL_SIO);
    if (plat_gpio_configure(gpio, PLAT_GPIO_OUT) < 0)
        return -1;
    return plat_gpio_write(gpio, 0);
}

#endif /* TOS_PWM */
