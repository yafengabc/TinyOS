#include "platform.h"
#include "hwdev.h"

/* The ATK-DNRP2350AM, as data.
 *
 * Every on-board peripheral this port can name is one line below: what it
 * is called, which class it belongs to, which pin it sits on, and how it is
 * wired.  /proc/dev prints this table, the kernel turns the LED / key /
 * display rows into /dev nodes, and nothing else in the tree hard-codes a
 * pin for this board - a caller says "the LED", not "GPIO3".
 *
 * This file is the board's data and nothing else: the LED and key code that
 * reads it lives in board_devs.c, which is shared with any variant of the
 * same board (board/rpi-pico2/board_data.c replaces only this file).
 *
 * The pin numbers come from the schematic (docs/ATK-DNRP2350AM_V1.0.PDF,
 * sheets 1-2) and were checked against the vendor wiki:
 *
 *   LED      GPIO3,  active low: the cathode is on the pin, the anode
 *                    through 510R to 3V3, so 0 lights it.
 *   KEY0     GPIO2,  to GND through 1K, so 0 means pressed.  It is the
 *                    BOOT button as well - held at reset it enters the
 *                    ROM bootloader.
 *   LCD      ST7789V2 135x240 on SPI1: SCK 10, MOSI 11, DC 8, CS 9,
 *                    backlight 25 (high = on), reset tied to board RESET.
 *   TF card  SDIO on GPIO10..15 - the same two pins the panel uses for
 *            clock and data, which is why both cannot run at once.
 *   UART0    GPIO0 TX / GPIO1 RX into the on-board CH343P USB-serial.
 *   USB      native USB 1.1 device port (BOOTSEL + CDC console).
 *   ADC0..3  GPIO26..29, 12 bit, 3.3 V reference.
 *   QSPI     W25Q32 4 MiB boot flash, dedicated pins, not on the header.
 *
 * What is described but not driven: the card slot.  A working SD driver is
 * a SPI/SDIO stack of its own, and on this board it would collide with the
 * panel anyway.  It is in the table so /proc/dev tells the truth about the
 * hardware, and so a future driver has one place to claim. */

#define LED_PIN   3
#define KEY_PIN   2

static const board_dev_t atk_devs[] = {
    /* `detail` is written front-loaded and terse: it is the column a
     * 40-column panel shows and truncates, so the wiring that matters
     * ("spi1 10/11") comes before the prose.  The long explanations live
     * in the comment above and in docs/BOARD-ATK-DNRP2350AM.md. */
    { "led0", "blue LED", TOS_DEV_LED, TOS_DEV_ACTIVE_LOW, LED_PIN, -1,
      "low = lit" },
    { "key0", "KEY1 / BOOT", TOS_DEV_KEY, TOS_DEV_ACTIVE_LOW, KEY_PIN, -1,
      "low = pressed" },
    { "lcd0", "ST7789V2 135x240", TOS_DEV_DISPLAY, 0, 8, 9,
      "spi1 10/11 dc8 cs9 bl25" },
    { "tf0", "TF card slot", TOS_DEV_STORAGE, TOS_DEV_SHARED, 10, -1,
      "sdio 10-15, shares lcd" },
    { "uart0", "CH343P USB-serial", TOS_DEV_UART, 0, 0, -1,
      "tx0 rx1 115200" },
    { "usb0", "native USB 1.1", TOS_DEV_USB, 0, -1, -1,
      "bootsel + cdc" },
    { "adc0", "GPIO26 / ADC0", TOS_DEV_ADC, 0, 26, 0, "12 bit, 3.3 V" },
    { "adc1", "GPIO27 / ADC1", TOS_DEV_ADC, 0, 27, 1, "12 bit, 3.3 V" },
    { "adc2", "GPIO28 / ADC2", TOS_DEV_ADC, 0, 28, 2, "12 bit, 3.3 V" },
    { "adc3", "GPIO29 / ADC3", TOS_DEV_ADC, 0, 29, 3, "12 bit, 3.3 V" },
    { "flash0", "W25Q32 4 MiB", TOS_DEV_FLASH, 0, -1, -1,
      "qspi, boot + littlefs" },
};

const board_dev_t *plat_board_devs(int *count)
{
    if (count)
        *count = (int)(sizeof(atk_devs) / sizeof(atk_devs[0]));
    return atk_devs;
}

const char *plat_board_name(void)
{
    return "ATK-DNRP2350AM";
}
