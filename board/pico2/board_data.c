#include "platform.h"
#include "hwdev.h"

/* Raspberry Pi Pico 2, as data.
 *
 * The only file this board replaces from board/rp2350-generic: the device table.
 * Everything else - platform.c, layout.c, the linker script, the PICOBIN
 * header - is the silicon's and is compiled from the base directory through
 * `base = rp2350` in board.conf.  The LED/key code that reads this table
 * lives in board/rp2350-generic/board_devs.c and is shared.
 *
 * Pin facts (Pico 2 datasheet, the "Pinout" section):
 *
 *   LED      GP25, active HIGH: the LED sits between the pin and ground
 *            through a resistor, so 1 lights it.  This is the opposite
 *            polarity from the ATK board, and the reason the flag lives in
 *            the table instead of in code.
 *   UART0    GP0 TX / GP1 RX on the header.  The console itself is USB CDC;
 *            UART0 is the fallback and the bring-up probe.
 *   USB      native USB 1.1 device port (BOOTSEL + CDC console).
 *   ADC0..3  GP26..29, 12 bit, 3.3 V reference.
 *   QSPI     4 MiB boot flash on the dedicated pads.
 *
 * Absent on purpose: no panel, no card slot, no user key.  BOOTSEL is a
 * button wired to the flash chip select, not a GPIO, so it cannot appear
 * here as a key. */

#define LED_PIN 25

static const board_dev_t pico2_devs[] = {
    { "led0", "GP25 LED", TOS_DEV_LED, 0, LED_PIN, -1,
      "high = lit" },
    { "uart0", "GP0/GP1 UART", TOS_DEV_UART, 0, 0, -1,
      "tx0 rx1 115200" },
    { "usb0", "native USB 1.1", TOS_DEV_USB, 0, -1, -1,
      "bootsel + cdc" },
    { "adc0", "GPIO26 / ADC0", TOS_DEV_ADC, 0, 26, 0, "12 bit, 3.3 V" },
    { "adc1", "GPIO27 / ADC1", TOS_DEV_ADC, 0, 27, 1, "12 bit, 3.3 V" },
    { "adc2", "GPIO28 / ADC2", TOS_DEV_ADC, 0, 28, 2, "12 bit, 3.3 V" },
    { "adc3", "GPIO29 / ADC3", TOS_DEV_ADC, 0, 29, 3, "12 bit, 3.3 V" },
    { "flash0", "QSPI 4 MiB", TOS_DEV_FLASH, 0, -1, -1,
      "qspi, boot + littlefs" },
};

const board_dev_t *plat_board_devs(int *count)
{
    if (count)
        *count = (int)(sizeof(pico2_devs) / sizeof(pico2_devs[0]));
    return pico2_devs;
}

const char *plat_board_name(void)
{
    return "Raspberry Pi Pico 2";
}
