#include "platform.h"
#include "hwdev.h"

/* The Raspberry Pi Pico (RP2040), as data.
 *
 * TinyOS has no Linux-style DTS parser: this table is the equivalent
 * description.  /proc/dev prints it, the LED row also gets a /dev/led0 node,
 * and a future driver claims a row instead of hard-coding a pin.  Rows with
 * no driver yet are still listed: /proc/dev is meant to tell the truth about
 * what the silicon has, not just what the kernel currently uses.
 *
 * Pin facts from the Pico pinout (the 40-pin header; the chip has 30 GPIO):
 *
 *   LED      GP25, active high.  It is on the board, not on the header.
 *   UART0    GP0 TX / GP1 RX, wired to the on-board CH343 USB-serial.
 *   UART1    GP4 TX / GP5 RX on the header.
 *   SPI0     SCK=2 TX=3 RX=4 CSn=5.
 *   SPI1     SCK=10 TX=11 RX=12 CSn=13.
 *   I2C0     SDA=4 SCL=5 (alternates exist on other pins).
 *   I2C1     SDA=6 SCL=7.
 *   ADC0..3  GP26..GP29; the header exposes GP26/27/28, GP29 is chip-only.
 *   ADC4     the on-chip temperature sensor, not a pin.
 *   USB      native USB 1.1 device/host, dedicated D+/D- pins.
 *   Flash    2 MiB QSPI (the image and littlefs live here).
 *
 * The remaining rows describe SoC blocks that are real but have no pin of
 * their own: the two PIO blocks (four state machines each), the 12-channel
 * DMA, the PWM bank, both PLLs, the integer divider, the two interpolators,
 * the core LDO and the SWD port.  They are here so the device map matches
 * the chip, not because a driver is pretending to own them yet. */

#define PICO_LED_PIN 25

static const board_dev_t pico_devs[] = {
    { "led0",   "on-board LED",         TOS_DEV_LED,    0, PICO_LED_PIN, -1,
      "high = lit" },

    { "uart0",  "UART0 / CH343",        TOS_DEV_UART,   0, 0, 0,
      "tx0 rx1 115200" },
    { "uart1",  "UART1 header",         TOS_DEV_UART,   0, 4, 1,
      "tx4 rx5" },

    { "spi0",   "SPI0 header",          TOS_DEV_SPI,    0, 2, 0,
      "sck2 tx3 rx4 cs5" },
    { "spi1",   "SPI1 header",          TOS_DEV_SPI,    0, 10, 1,
      "sck10 tx11 rx12 cs13" },

    { "i2c0",   "I2C0 header",          TOS_DEV_I2C,    0, 4, 0,
      "sda4 scl5" },
    { "i2c1",   "I2C1 header",          TOS_DEV_I2C,    0, 6, 1,
      "sda6 scl7" },

    { "adc0",   "GP26 / ADC0",          TOS_DEV_ADC,    0, 26, 0,
      "12 bit, 3.3 V" },
    { "adc1",   "GP27 / ADC1",          TOS_DEV_ADC,    0, 27, 1,
      "12 bit, 3.3 V" },
    { "adc2",   "GP28 / ADC2",          TOS_DEV_ADC,    0, 28, 2,
      "12 bit, 3.3 V" },
    { "adc3",   "GP29 / ADC3",          TOS_DEV_ADC,    0, 29, 3,
      "12 bit, chip-only pin" },
    { "adc4",   "on-chip temperature",  TOS_DEV_ADC,    0, -1, 4,
      "ADC4, 12 bit" },

    { "pwm0",   "8 slices / 16 channels", TOS_DEV_PWM,  0, -1, 0,
      "gpio0-29" },
    { "pio0",   "PIO0",                 TOS_DEV_PIO,    0, -1, 0,
      "4 state machines" },
    { "pio1",   "PIO1",                 TOS_DEV_PIO,    0, -1, 1,
      "4 state machines" },
    { "dma0",   "12-channel DMA",       TOS_DEV_DMA,    0, -1, 0,
      "chain + pacing" },

    { "usb0",   "native USB 1.1",       TOS_DEV_USB,    0, -1, -1,
      "device/host, cdc" },
    { "flash0", "2 MiB QSPI",           TOS_DEV_FLASH,  0, -1, -1,
      "boot + littlefs" },
    { "gpio0",  "RP2040 GPIO bank",     TOS_DEV_GPIO,   0, -1, -1,
      "30 chip, 26 header" },

    { "pll_sys", "PLL_SYS",             TOS_DEV_PLL,    0, -1, 0,
      "12 MHz -> 125 MHz" },
    { "pll_usb", "PLL_USB",             TOS_DEV_PLL,    0, -1, 1,
      "12 MHz -> 48 MHz" },
    { "div0",   "integer divider",      TOS_DEV_DIV,    0, -1, 0,
      "SIO, 8-cycle" },
    { "interp0", "interpolator 0",      TOS_DEV_INTERP, 0, -1, 0,
      "2 lanes" },
    { "interp1", "interpolator 1",      TOS_DEV_INTERP, 0, -1, 1,
      "2 lanes" },
    { "ldo0",   "on-chip LDO",          TOS_DEV_LDO,    0, -1, -1,
      "1.1 V core" },
    { "swd0",   "SWD debug",            TOS_DEV_SWD,    0, -1, -1,
      "swclk/swdio pads" },
};

const board_dev_t *plat_board_devs(int *count)
{
    if (count)
        *count = (int)(sizeof(pico_devs) / sizeof(pico_devs[0]));
    return pico_devs;
}

const char *plat_board_name(void)
{
    return "Raspberry Pi Pico (RP2040)";
}

/* ---- LED -------------------------------------------------------------- */

/* GP25 is the Pico's only user-visible light and is active high.  Going
 * through the pin layer (rather than poking SIO here) keeps the pad and
 * function-select rules in one place; the boot code may have already
 * configured the pin, and plat_gpio_configure() is idempotent. */

static int led_ready;
static int led_state;

int plat_led_count(void)
{
    return 1;
}

int plat_led_set(int idx, int on)
{
    if (idx != 0)
        return -1;
    if (!led_ready) {
        if (plat_gpio_configure(PICO_LED_PIN, PLAT_GPIO_OUT) < 0)
            return -1;
        led_ready = 1;
    }
    if (plat_gpio_write(PICO_LED_PIN, on ? 1 : 0) < 0)
        return -1;
    led_state = on ? 1 : 0;
    return 0;
}

int plat_led_get(int idx)
{
    return idx == 0 ? led_state : -1;
}
