#include "lib.h"

/* pwm - drive one GPIO as a PWM output.
 *
 *   pwm                     describe the PWM block and this usage
 *   pwm <gpio> <hz> <pct>   configure a pin
 *   pwm <gpio> off          stop and release the pin
 *
 * The RP2040 has eight slices with two outputs each (16 channels), and any
 * GPIO 0..29 can be routed to one of them.  Both outputs of a slice share
 * the frequency, so the command prints which slice a pin landed on.
 */

static void usage(void)
{
    puts("usage: pwm <gpio> <hz> <duty%>   |   pwm <gpio> off\n");
}

int main(int argc, char *argv[])
{
    int n = pwm_count();
    int gpio, slice, freq, pct, duty;
    char ch;

    if (n <= 0) {
        puts("pwm: this board has no PWM wired up for user space\n");
        return 1;
    }

    if (argc < 2) {
        printf("pwm: %d channels, GPIO 0..29\n", n);
        usage();
        return 0;
    }

    gpio = atoi(argv[1]);
    if (gpio < 0 || gpio >= 30) {
        printf("pwm: GPIO %d does not exist (0..29)\n", gpio);
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

    slice = (gpio >> 1) & 7;
    ch = (gpio & 1) ? 'B' : 'A';
    printf("pwm: GP%d -> slice %d%c, %d Hz, %d%%\n",
           gpio, slice, ch, freq, pct);
    return 0;
}
