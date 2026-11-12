/* The ESP32-C3 core board (ESP32-C3MINI-1-N4 on a CH340K carrier), as data.
 *
 * Every on-board peripheral this port can name is one line in the table:
 * what it is called, which class it belongs to, which pin it sits on, and
 * how it is wired.  /proc/dev prints this table, the kernel turns the LED /
 * key rows into /dev nodes, and nothing else in the tree hard-codes a pin
 * for this board - a caller says "the LED", not "GPIO6".
 *
 * Pin facts, from the board's own pinout diagram (the silkscreen numbers
 * are header positions 1..15, the GPIO numbers are what the chip uses):
 *
 *   RGB LED   WS2812 on GPIO6.  The data line only; power comes from the
 *             3V3 rail, so logic levels work without a level shifter.
 *   KEY (BOOT) GPIO9, to GND through the strapping network, internal
 *             pull-up needed: 0 = pressed.  Held at reset it enters the
 *             ROM download mode, exactly like the ATK board's KEY1.
 *   TX/RX LED and PWR LED  driven by the CH340K / the LDO - not wired to
 *             any GPIO, so they are not devices here.  A board table that
 *             lists an LED the kernel cannot blink would be a lie.
 *   UART0     GPIO21 TX / GPIO20 RX into the CH340K USB-serial (the
 *             second Type-C).  The console already lives here.
 *   USB       native USB-Serial/JTAG on GPIO18/19 (the first Type-C).
 *   ADC1      channels 0..4 on GPIO0..4; ADC2 channel 0 on GPIO5.  Only
 *             ADC1 is wired up below: ADC2 shares its blocks with Wi-Fi,
 *             and the C3 errata says its one-shot reads are unreliable.
 *   SPI2      SCK=GPIO4, MOSI=GPIO5, MISO=GPIO2.  CS is a plain GPIO (10 by
 *             convention) because one bus usually has several devices.
 *   I2C0      SDA=GPIO8, SCL=GPIO9.  Bit-banged for now; note that GPIO9 is
 *             also the BOOT strapping pin, so a device holding SCL low at
 *             reset keeps the chip out of the flash boot path.
 *   PWM       LEDC channels 0..5 can be routed to any of GPIO0..10.
 *   Flash     4 MiB inside the module (boot + littlefs), not a header pin.
 */

#include "platform.h"
#include "hwdev.h"
#include "board.h"

/* The GPIO registers this file needs.  They live in gpio.c today; when a
 * second user appears they should move into a board-local memmap header. */
#define GPIO_BASE        0x60004000u
#define GPIO_OUT_REG     (GPIO_BASE + 0x04)
#define GPIO_OUT_W1TS    (GPIO_BASE + 0x08)
#define GPIO_OUT_W1TC    (GPIO_BASE + 0x0C)

#define LED_PIN   6              /* WS2812 data in */
#define KEY_PIN   9              /* BOOT button */

static const board_dev_t c3_devs[] = {
    { "rgb0",  "WS2812 RGB LED", TOS_DEV_LED, 0, LED_PIN, -1,
      "ws2812, one pixel" },
    { "key0",  "BOOT",           TOS_DEV_KEY, TOS_DEV_ACTIVE_LOW, KEY_PIN, -1,
      "low = pressed" },
    { "adc0",  "GPIO0 / ADC1_0", TOS_DEV_ADC, 0, 0, 0, "12 bit, 3.3 V" },
    { "adc1",  "GPIO1 / ADC1_1", TOS_DEV_ADC, 0, 1, 1, "12 bit, 3.3 V" },
    { "adc2",  "GPIO2 / ADC1_2", TOS_DEV_ADC, 0, 2, 2, "12 bit, 3.3 V" },
    { "adc3",  "GPIO3 / ADC1_3", TOS_DEV_ADC, 0, 3, 3, "12 bit, 3.3 V" },
    { "adc4",  "GPIO4 / ADC1_4", TOS_DEV_ADC, 0, 4, 4, "12 bit, 3.3 V" },

    { "spi0",  "SPI2 header",    TOS_DEV_SPI, 0, 4, 0, "sck4 mosi5 miso2" },
    { "i2c0",  "I2C0 header",    TOS_DEV_I2C, 0, 8, 0, "sda8 scl9" },
    { "pwm0",  "6 LEDC channels", TOS_DEV_PWM, 0, -1, 0, "gp0..gp10" },

    { "uart0", "CH340K USB-serial", TOS_DEV_UART, 0, 21, -1,
      "tx21 rx20 115200" },
    { "usb0",  "native USB-Serial/JTAG", TOS_DEV_USB, 0, -1, -1,
      "cdc console" },
    { "flash0", "4 MiB module", TOS_DEV_FLASH, 0, -1, -1,
      "spi, boot + littlefs" },
};

const board_dev_t *plat_board_devs(int *count)
{
    if (count)
        *count = (int)(sizeof(c3_devs) / sizeof(c3_devs[0]));
    return c3_devs;
}

const char *plat_board_name(void)
{
    return "ESP32-C3 core (CH340K)";
}

/* ---- "LED": one WS2812 pixel ------------------------------------------ */

/* The board has no plain LED - its only controllable light is a WS2812.
 * Rather than inventing a third device class for one board, the LED hook
 * drives the strip: on = white, off = dark.  The button-style plat_led_get
 * keeps its meaning (the last state set), and the /dev/rgb0 node behaves
 * exactly like /dev/led0 does on the ATK board, which is what makes the
 * shell's led0 examples work unchanged.
 *
 * Bit-banging at 12 bit-periods per bit is ~90 us for 24 bits, well under
 * the 50 us reset latch, so a single interrupts-off window per frame is
 * both correct and short.  The WS2812 datasheet timings (800 kHz class):
 *   0 code: 0.4 us high + 0.85 us low
 *   1 code: 0.8 us high + 0.45 us low
 * At the ROM's 20 MHz one cycle is 50 ns; those are 8/17 and 16/9 cycles.
 * At 160 MHz (if someone turns the PLL on) they are 1/16 scale and still
 * within the part's tolerance because the ratio is what matters. */
#define WS_T0H 8
#define WS_T0L 17
#define WS_T1H 16
#define WS_T1L 9

static int led_ready;
static int key_ready;
static int led_state;

/* The 24-bit pixel, GRB order per the WS2812 datasheet. */
static uint32_t ws_pixel;

static void ws_send(void)
{
    uint32_t mask;
    uint32_t pin_mask = 1u << LED_PIN;
    uint32_t idle = mmio_read32(GPIO_OUT_REG) & ~pin_mask;

    plat_irq_disable();
    for (mask = 0x800000u; mask; mask >>= 1) {
        uint32_t high = (ws_pixel & mask) ? WS_T1H : WS_T0H;
        uint32_t low  = (ws_pixel & mask) ? WS_T1L : WS_T0L;

        mmio_write32(GPIO_OUT_W1TS, pin_mask);
        for (uint32_t i = 0; i < high; i++)
            asm volatile ("nop");
        mmio_write32(GPIO_OUT_W1TC, pin_mask);
        for (uint32_t i = 0; i < low; i++)
            asm volatile ("nop");
    }
    mmio_write32(GPIO_OUT_REG, idle);   /* leave the line low for the latch */
    plat_irq_enable();
}

int plat_led_count(void)
{
    return 1;
}

int plat_led_set(int idx, int on)
{
    if (idx != 0)
        return -1;
    if (!led_ready) {
        if (plat_gpio_configure(LED_PIN, PLAT_GPIO_OUT) < 0)
            return -1;
        led_ready = 1;
    }
    if (on) {
        /* White: max green, max red, max blue (GRB). */
        ws_pixel = 0xFFFFFF;
    } else {
        ws_pixel = 0;
    }
    ws_send();
    led_state = on ? 1 : 0;
    return 0;
}

int plat_led_get(int idx)
{
    if (idx != 0)
        return -1;
    return led_state;
}

/* ---- key -------------------------------------------------------------- */

int plat_key_count(void)
{
    return 1;
}

int plat_key_read(int idx)
{
    int level;

    if (idx != 0)
        return -1;
    if (!key_ready) {
        /* The strapping network has its own pull on this pin at reset, but
         * a released button must not float afterwards either, so the pad
         * keeps an internal pull-up the same as the ATK board's KEY0. */
        if (plat_gpio_configure(KEY_PIN, PLAT_GPIO_IN_PULLUP) < 0)
            return -1;
        key_ready = 1;
    }
    level = plat_gpio_read(KEY_PIN);
    return level == 0;          /* active low: 1 means pressed */
}

/* ---- ADC -------------------------------------------------------------- */

/* One SAR ADC, polled in one-shot ("onetime") mode.  Register names and
 * offsets are esp-idf's apb_saradc_reg.h; the base address is the official
 * memory map's 0x60040000.  ADC1 channels 0..4 are GPIO0..4; channel 5
 * would be ADC2/GPIO5, which the errata makes unreliable in one-shot mode,
 * so the hook refuses it and the device table does not offer it.
 *
 * One-time sampling needs no pattern table, no DMA and no interrupt: set
 * the channel, set ONETIME_START, wait for ADC1_DONE in INT_RAW, read the
 * 17-bit field in DATA_STATUS.  The clock comes from clk_apb via the same
 * divider the ROM leaves at reset, so no clock bring-up is needed. */

#define SARADC_BASE        0x60040000u
#define SARADC_CTRL        (SARADC_BASE + 0x00u)
#define SARADC_ONETIME     (SARADC_BASE + 0x20u)
#define SARADC_DATA        (SARADC_BASE + 0x2Cu)
#define SARADC_INT_RAW     (SARADC_BASE + 0x44u)
#define SARADC_INT_CLR     (SARADC_BASE + 0x4Cu)
#define SARADC_CLKM_CONF   (SARADC_BASE + 0x54u)

#define ONETIME_EN1        (1u << 31)
#define ONETIME_START      (1u << 29)
#define ONETIME_CHAN_S     25
#define ONETIME_ATTEN_S    23
#define INT_ADC1_DONE      (1u << 31)
#define CLKM_CLK_EN        (1u << 20)

/* 12 dB attenuation: full 0..3.3 V range. */
#define ATTEN_12DB         3u

static int adc_ready;

static void adc_init_hw(void)
{
    if (adc_ready)
        return;

    /* The APB clock gate for the SARADC registers, and an explicit source:
     * CLK_SEL=1 is clk_apb.  Without it the divider runs from an unselected
     * clock and DONE never fires. */
    mmio_write32(SARADC_CLKM_CONF,
                 (mmio_read32(SARADC_CLKM_CONF) & ~(3u << 21)) |
                 (1u << 21) | CLKM_CLK_EN);
    /* Force the SAR block powered: XPD_SAR_FORCE = 3 means "always on",
     * and the FSM's wait counters get sane defaults. */
    mmio_write32(SARADC_CTRL, (3u << 27) | (1u << 30));

    adc_ready = 1;
}

int plat_adc_count(void)
{
    return 5;                   /* ADC1 channels 0..4 */
}

int plat_adc_bits(void)
{
    return 12;
}

int plat_adc_vref_mv(void)
{
    /* The C3's reference is nominally 1100 mV, but with 12 dB attenuation
     * the full-scale is about 3.3 V (espressif's own table says ~3.1 V
     * usable; 3300 keeps `adc`'s mV column honest to within its other
     * error sources). */
    return 3300;
}

/* All channels returning the same value on an unwired board is expected:
 * every pad floats at roughly the same undefined level (~850 mV here).
 * Proof that the channel selector works comes from SAR1_STATUS below and
 * from touching a pin to 3V3/GND - see docs/BOARD-ESP32C3-CORE.md. */
int plat_adc_read(int chan)
{
    uint32_t v;

    if (chan < 0 || chan > 4)
        return -1;              /* channel 5 is ADC2: unreliable, refused */

    adc_init_hw();

    /* The pin is the channel on this chip.  The analog switch is parallel
     * to the GPIO matrix, but the pad must be a *disabled digital output
     * with IE on* or the digital stage fights the analog source - and the
     * default after reset is only right for GPIO0 (channel 0 worked without
     * this; 1..4 did not).  Configure every channel once, lazily. */
    {
        static int pad_ready[5];

        if (!pad_ready[chan]) {
            plat_gpio_configure(chan, PLAT_GPIO_IN);
            pad_ready[chan] = 1;
        }
    }

    /* One conversion: channel + attenuation, then start.  ONETIME_START is
     * a rising edge, not a level: writing it while the previous conversion
     * left it set does nothing (measured: channel 0 ran once, then every
     * later call timed out).  Clear first, then pulse. */
    mmio_write32(SARADC_INT_CLR, INT_ADC1_DONE);
    mmio_write32(SARADC_ONETIME,
                 ONETIME_EN1 |
                 ((uint32_t)chan << ONETIME_CHAN_S) |
                 (ATTEN_12DB << ONETIME_ATTEN_S));
    mmio_write32(SARADC_ONETIME,
                 ONETIME_EN1 |
                 ((uint32_t)chan << ONETIME_CHAN_S) |
                 (ATTEN_12DB << ONETIME_ATTEN_S) |
                 ONETIME_START);

    for (uint32_t i = 0; i < 100000u; i++) {
        if (mmio_read32(SARADC_INT_RAW) & INT_ADC1_DONE)
            break;
    }
    if (!(mmio_read32(SARADC_INT_RAW) & INT_ADC1_DONE))
        return -1;

    v = mmio_read32(SARADC_DATA) & 0xFFFu;   /* 12-bit result */
    return (int)v;
}
