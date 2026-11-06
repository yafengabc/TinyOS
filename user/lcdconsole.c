#include "lib.h"

/* lcdconsole - switch the LCD's copy of the console on and off.
 *
 *   lcdconsole          show the current state
 *   lcdconsole on        console output is mirrored onto the panel
 *   lcdconsole off       the panel keeps whatever it is showing
 *
 * Handy around a framebuffer program: turn mirroring off first so the
 * shell prompt does not scroll the picture away, then draw with `fb`.
 * `screen mirror on|off` does the same thing; this command is the short
 * spelling, plus a status query. */

int main(int argc, char **argv)
{
    int fd = open("/dev/lcd0");
    int prev, want;

    if (fd < 0) {
        puts("lcdconsole: this board has no display\n");
        return 1;
    }

    if (argc < 2) {
        prev = ioctl(fd, LCD_OP_MIRROR_GET, 0);
        printf("lcd console: %s\n", prev ? "on" : "off");
        printf("usage: lcdconsole on|off\n");
        close(fd);
        return 0;
    }

    if (!strcmp(argv[1], "on") || !strcmp(argv[1], "1"))
        want = 1;
    else if (!strcmp(argv[1], "off") || !strcmp(argv[1], "0"))
        want = 0;
    else {
        puts("usage: lcdconsole [on|off]\n");
        close(fd);
        return 2;
    }

    prev = ioctl(fd, LCD_OP_MIRROR, want);
    printf("lcd console: %s -> %s\n",
           prev ? "on" : "off", want ? "on" : "off");
    close(fd);
    return 0;
}
