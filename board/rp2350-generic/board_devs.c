#include "platform.h"
#include "hwdev.h"
#include "board.h"

/* LED and key, driven by the board's device table.
 *
 * The pins and the polarity are board data (board_data.c, one table per
 * board or variant); this file only knows how to turn a row into a pin
 * operation.  That split is what lets board/rpi-pico2 reuse it: its LED is on
 * GPIO25 and active high, which is a different row, not different code.
 *
 * Both are written on top of platform.h's pin layer rather than poking
 * PADS_BANK0 directly.  A GPIO class is a pin with a meaning: the pad bits
 * (pull-up, output-disable, the RP2350 ISO bit) are already right in one
 * place, and a board file that re-derives them slowly drifts from the one
 * that is actually tested. */

#define MAX_LEDS 2
#define MAX_KEYS 2

static int led_ready[MAX_LEDS];
static int key_ready[MAX_KEYS];
static int led_state[MAX_LEDS];
static int led_owned;           /* user space has taken the LED over */

static const board_dev_t *dev_by_class(uint8_t cls, int idx)
{
    int count = 0;
    const board_dev_t *devs = plat_board_devs(&count);

    if (!devs || idx < 0)
        return NULL;
    for (int i = 0; i < count; i++) {
        if (devs[i].cls != cls)
            continue;
        if (idx-- == 0)
            return &devs[i];
    }
    return NULL;
}

static int dev_count_by_class(uint8_t cls)
{
    int count = 0;
    int n = 0;
    const board_dev_t *devs = plat_board_devs(&count);

    if (!devs)
        return 0;
    for (int i = 0; i < count; i++)
        if (devs[i].cls == cls)
            n++;
    return n;
}

/* The one place polarity stops: every caller above says "on". */
static int dev_level(const board_dev_t *d, int on)
{
    if (d->flags & TOS_DEV_ACTIVE_LOW)
        return on ? 0 : 1;
    return on ? 1 : 0;
}

int plat_led_count(void)
{
    int n = dev_count_by_class(TOS_DEV_LED);
    return n > MAX_LEDS ? MAX_LEDS : n;
}

int plat_led_set(int idx, int on)
{
    const board_dev_t *d = dev_by_class(TOS_DEV_LED, idx);

    if (!d || d->pin < 0 || idx >= MAX_LEDS)
        return -1;
    if (!led_ready[idx]) {
        if (plat_gpio_configure(d->pin, PLAT_GPIO_OUT) < 0)
            return -1;
        led_ready[idx] = 1;
    }
    if (plat_gpio_write(d->pin, dev_level(d, on ? 1 : 0)) < 0)
        return -1;
    led_state[idx] = on ? 1 : 0;
    /* Stop the heartbeat: from here the LED says what user space told it
     * to say, not "the kernel is alive". */
    led_owned = 1;
    return 0;
}

int plat_led_get(int idx)
{
    if (idx < 0 || idx >= MAX_LEDS)
        return -1;
    if (!dev_by_class(TOS_DEV_LED, idx))
        return -1;
    return led_state[idx];
}

int plat_led_owned(void)
{
    return led_owned;
}

int plat_key_count(void)
{
    int n = dev_count_by_class(TOS_DEV_KEY);
    return n > MAX_KEYS ? MAX_KEYS : n;
}

int plat_key_read(int idx)
{
    const board_dev_t *d = dev_by_class(TOS_DEV_KEY, idx);
    int level;

    if (!d || d->pin < 0 || idx >= MAX_KEYS)
        return -1;
    if (!key_ready[idx]) {
        /* Pull-up: the switch is wired straight to ground, so a released
         * key floats without it. */
        if (plat_gpio_configure(d->pin, PLAT_GPIO_IN_PULLUP) < 0)
            return -1;
        key_ready[idx] = 1;
    }
    level = plat_gpio_read(d->pin);
    if (level < 0)
        return -1;
    /* 1 means pressed, whatever the wiring: an active-low key reads 0 when
     * the button pulls it down. */
    return (d->flags & TOS_DEV_ACTIVE_LOW) ? (level ? 0 : 1) : (level ? 1 : 0);
}
