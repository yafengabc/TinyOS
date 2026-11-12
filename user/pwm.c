#include "lib.h"

/* pwm - drive one GPIO as a PWM output.
 *
 *   pwm                     describe the PWM block and this usage
 *   pwm <gpio> <hz> <pct>   configure a pin
 *   pwm <gpio> off          stop and release the pin
 *
 * Which pins can do PWM is board policy: the RP2040 maps GPIOs to slices
 * (two outputs of a slice share a frequency), the ESP32-C3 routes six LEDC
 * channels through the GPIO matrix.  `cat /proc/dev` lists what this board
 * exposes; a pin the driver cannot use is rejected with an error.
 */

static void usage(void)
{
    puts("usage: pwm <gpio> <hz> <duty%>   |   pwm <gpio> off\n");
}

int main(int argc, char *argv[])
{
    int n = pwm_count();
    int gpio, freq, pct, duty;

    if (n <= 0) {
        puts("pwm: this board has no PWM wired up for user space\n");
        return 1;
    }

    if (argc < 2) {
        printf("pwm: %d channels; 'cat /proc/dev' lists the pins\n", n);
        usage();
        return 0;
    }

    gpio = atoi(argv[1]);
    if (gpio < 0 || gpio >= gpio_count()) {
        printf("pwm: GPIO %d does not exist (0..%d)\n", gpio,
               gpio_count() - 1);
        return 1;
    }

    if (argc == 3 && !strcmp(argv[2], "off")) {
        if (pwm_off(gpio) < 0) {
            printf("pwm: cannot stop GP%d\n", gpio);
            return 1;
        }
        printf("pwm: GP%d off\n", gpio);
        return 0;
    }

    if (argc < 4) {
        usage();
        return 1;
    }

    freq = atoi(argv[2]);
    pct  = atoi(argv[3]);
    if (freq <= 0 || pct < 0 || pct > 100) {
        printf("pwm: bad frequency/duty (need hz > 0, 0 <= duty%% <= 100)\n");
        return 1;
    }

    duty = pct * 10;
    if (pwm_set(gpio, freq, duty) < 0) {
        printf("pwm: cannot set GP%d to %d Hz / %d%%\n", gpio, freq, pct);
        return 1;
    }

    printf("pwm: GP%d = %d Hz, %d%%\n", gpio, freq, pct);
    return 0;
}
