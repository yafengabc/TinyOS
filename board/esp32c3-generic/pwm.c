#include "platform.h"
#include "board.h"
#include "board_internal.h"

#ifdef TOS_PWM

/* PWM via the ESP32-C3 LEDC block.
 *
 * The C3 only has the low-speed half of the LED PWM controller: six channels
 * (LSCH0..5) driven by four timers (LSTIMER0..3), each with its own divider.
 * A channel is not tied to a pin: the GPIO matrix routes LEDC_LS_SIG_OUTn to
 * any pad, so the API stays pin-based like the RP2040 port.
 *
 *   pin -> channel = gpio % 6, timer = channel % 4
 *
 * Two pins that land on the same channel share it (the later call wins), and
 * two channels on the same timer share the frequency - the same rule the
 * RP2040's slices have, just with a different mapping.
 *
 * Clock: the timer counts APB_CLK (LEDC_CONF.APB_CLK_SEL = 0, tick_sel = 0).
 * The divider is 18 bits with 8 fractional bits, so
 *
 *   div = APB / (freq * 2^res)
 *
 * and `res` is chosen as the largest value that keeps that divider >= 1.
 * Duty is a 19-bit field; the hardware compares against the timer count, so
 * the value is written shifted left by 4 (esp-idf does the same).
 */

#define LEDC_BASE          0x60019000u

#define LEDC_CONF          (LEDC_BASE + 0xd0u)
#define LEDC_CH_CONF0(n)   (LEDC_BASE + 0x00u + 0x14u * (uint32_t)(n))
#define LEDC_CH_HPOINT(n)  (LEDC_BASE + 0x04u + 0x14u * (uint32_t)(n))
#define LEDC_CH_DUTY(n)    (LEDC_BASE + 0x08u + 0x14u * (uint32_t)(n))
#define LEDC_CH_CONF1(n)   (LEDC_BASE + 0x0cu + 0x14u * (uint32_t)(n))
#define LEDC_TIMER_CONF(t) (LEDC_BASE + 0xa0u + 8u * (uint32_t)(t))

#define CONF0_TIMER_SEL_M  0x3u
#define CONF0_SIG_OUT_EN   (1u << 2)
#define CONF0_PARA_UP      (1u << 4)

#define CONF1_DUTY_START   (1u << 31)

#define TIMER_DUTY_RES_M   0xfu
#define TIMER_CLK_DIV_M    (0x3ffffu << 4)
#define TIMER_PAUSE        (1u << 22)
#define TIMER_RST          (1u << 23)
#define TIMER_TICK_SEL     (1u << 24)      /* 1 = REF_TICK, 0 = APB */
#define TIMER_PARA_UP      (1u << 25)

#define LEDC_CONF_CLK_EN   (1u << 31)      /* register-file clock gate */
#define LEDC_MAX_RES       14

#define SYSTEM_BASE        0x60026000u
#define PERIP_CLK_EN0      (SYSTEM_BASE + 0x10u)
#define PERIP_RST_EN0      (SYSTEM_BASE + 0x18u)
#define PERIP_LEDC         (1u << 11)

#define GPIO_BASE          0x60004000u
#define GPIO_FUNC_OUT(n)   (GPIO_BASE + 0x554u + 4u * (uint32_t)(n))

#define SIG_LEDC_LS0       45u             /* LEDC_LS_SIG_OUT0_IDX */

static int g_ready;

/* Header pins only: 11..17 are the SPI flash, 18/19 are USB and 20/21 are
 * the console UART.  A PWM on any of those would break the board. */
static int pwm_pin_ok(int gpio)
{
    return gpio >= 0 && gpio <= 10;
}

static void ledc_init(void)
{
    if (g_ready)
        return;

    mmio_write32(PERIP_CLK_EN0, mmio_read32(PERIP_CLK_EN0) | PERIP_LEDC);
    mmio_write32(PERIP_RST_EN0, mmio_read32(PERIP_RST_EN0) & ~PERIP_LEDC);
    /* CLK_EN must be set before any other register in the block responds;
     * APB_CLK_SEL = 0 keeps the timers on APB_CLK. */
    mmio_write32(LEDC_CONF, LEDC_CONF_CLK_EN);
    g_ready = 1;
}

int plat_pwm_count(void)
{
    return 6;
}

int plat_pwm_set(int gpio, int freq_hz, int duty_permille)
{
    uint32_t apb, div, res, duty, chan, timer;

    if (!pwm_pin_ok(gpio) || freq_hz <= 0 ||
        duty_permille < 0 || duty_permille > 1000)
        return -1;
    if (plat_gpio_configure(gpio, PLAT_GPIO_OUT) < 0)
        return -1;

    ledc_init();
    apb = c3_apb_hz();
    if (apb == 0)
        return -1;

    for (res = LEDC_MAX_RES; res > 0; res--)
        if (((uint64_t)freq_hz << res) <= (uint64_t)apb)
            break;
    if (res == 0)
        return -1;                      /* frequency above APB/2 */

    div = ((uint64_t)apb << 8) / ((uint64_t)freq_hz << res);
    if (div < 0x100u)
        div = 0x100u;
    if (div > 0x3ffffu)
        return -1;

    chan  = (uint32_t)gpio % 6u;
    timer = chan % 4u;

    /* Timer: reset the counter, then latch resolution/divider with PARA_UP.
     * PAUSE stays 0, TICK_SEL stays 0 (APB). */
    mmio_write32(LEDC_TIMER_CONF(timer),
                 (res & TIMER_DUTY_RES_M) | ((div & 0x3ffffu) << 4) | TIMER_RST);
    mmio_write32(LEDC_TIMER_CONF(timer),
                 (res & TIMER_DUTY_RES_M) | ((div & 0x3ffffu) << 4) | TIMER_PARA_UP);

    duty = (uint32_t)(((uint64_t)duty_permille * (((uint32_t)1u << res) - 1u)) / 1000u);

    mmio_write32(LEDC_CH_HPOINT(chan), 0);
    mmio_write32(LEDC_CH_DUTY(chan), (duty << 4) & 0x7ffffu);
    mmio_write32(LEDC_CH_CONF1(chan), CONF1_DUTY_START);
    mmio_write32(LEDC_CH_CONF0(chan), timer | CONF0_SIG_OUT_EN | CONF0_PARA_UP);

    /* Route the channel's output signal to this pad.  Clearing the OEN_SEL
     * bit (bit 8) makes the pad's output enable come from the LEDC, not
     * from GPIO_ENABLE. */
    mmio_write32(GPIO_FUNC_OUT(gpio), SIG_LEDC_LS0 + chan);
    return 0;
}

int plat_pwm_off(int gpio)
{
    uint32_t chan, timer;

    if (!pwm_pin_ok(gpio))
        return -1;

    chan  = (uint32_t)gpio % 6u;
    timer = chan % 4u;
    /* Drop sig_out_en, then hand the pad back to the GPIO peripheral. */
    mmio_write32(LEDC_CH_CONF0(chan), timer | CONF0_PARA_UP);

    if (plat_gpio_configure(gpio, PLAT_GPIO_OUT) < 0)
        return -1;
    plat_gpio_write(gpio, 0);
    return 0;
}

#endif /* TOS_PWM */
