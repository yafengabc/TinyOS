#include "lib.h"

/* splash - a hand-run title card for the LCD, deliberately NOT part of boot.
 *
 * The boot path must stay fast, so this is just another user program: open
 * /dev/lcd0, silence the console mirror (otherwise the shell prompt would
 * immediately scroll the picture away), draw a 40x16 ASCII card, and exit.
 * The panel keeps the picture until `splash off` restores the mirror.
 *
 * Everything goes through the same open/write/ioctl interface as `screen`,
 * so it works unchanged on any board that publishes a display. */

#define SPLASH_FG 0x07FFu        /* cyan, RGB565 */
#define SPLASH_BG 0x0000u        /* black        */

/* Exactly 16 rows, none reaching 40 columns: putc_locked() wraps at 40, so
 * a full-width line followed by '\n' would skip a row, and a final newline
 * on row 15 would scroll the whole wordmark off the top.  The last line
 * deliberately has no newline. */
static const char card[] =
    "\n"
    "  ##### ##### #   # #   #  ###   ####\n"
    "    #     #   ##  #  # #  #   # #    \n"
    "    #     #   # # #   #   #   #  ### \n"
    "    #     #   #  ##   #   #   #     #\n"
    "    #   ##### #   #   #    ###  #### \n"
    "\n"
    "  ====================================\n"
    "\n"
    "  a tiny multi-platform OS\n"
    "  -----------------------------------\n"
    "\n"
    "  user ELF / PIE   littlefs   /dev/lcd0\n"
    "  console: uart0 115200   usb0 cdc\n"
    "\n"
    "  run 'splash off' to restore console";

static int lcd_open(void)
{
    int fd = open("/dev/lcd0");

    if (fd < 0)
        puts("splash: this board has no display (or the driver did not bind)\n");
    return fd;
}

int main(int argc, char **argv)
{
    int fd = lcd_open();

    if (fd < 0)
        return 1;

    /* `splash off`: hand the glass back to the console. */
    if (argc > 1 && !strcmp(argv[1], "off")) {
        ioctl(fd, LCD_OP_FG, 0xFFFFu);
        ioctl(fd, LCD_OP_CLEAR, SPLASH_BG);
        ioctl(fd, LCD_OP_MIRROR, 1);
        close(fd);
        puts("splash: console mirror restored\n");
        return 0;
    }

    if (argc > 1) {
        puts("usage: splash          draw the title card\n");
        puts("       splash off     restore the console mirror\n");
        close(fd);
        return 2;
    }

    /* Colour and clear first: screen_colors() repaints the whole panel.
     * The mirror goes off after that, before the card is written. */
    ioctl(fd, LCD_OP_CLEAR, SPLASH_BG);
    ioctl(fd, LCD_OP_FG, SPLASH_FG);
    ioctl(fd, LCD_OP_BG, SPLASH_BG);
    ioctl(fd, LCD_OP_CURSOR, 0);
    ioctl(fd, LCD_OP_BACKLIGHT, 1);
    ioctl(fd, LCD_OP_MIRROR, 0);

    if (write(fd, card, sizeof(card) - 1) < 0)
        puts("splash: panel write failed\n");

    close(fd);
    puts("splash: title card shown; run 'splash off' to restore console\n");
    return 0;
}
