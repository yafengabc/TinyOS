#include "lib.h"

/* screen - the board's panel, as a terminal.
 *
 *   screen                     status card: what this machine is
 *   screen dev                 the board's device table, on the panel
 *   screen text <words...>     write a line at the cursor
 *   screen clear [color]       fill with a colour (name or 0xRRGGBB)
 *   screen cursor <col> <row>
 *   screen mirror on|off       mirror the console onto the panel
 *   screen invert on|off       swap foreground and background
 *   screen backlight on|off
 *   screen info                geometry and current settings
 *
 * Everything goes through /dev/lcd0 - open, write, ioctl - so the same
 * commands would work against any board whose device table names a
 * display.  There is no C API for the screen on purpose: a screen is not a
 * peripheral you poll, it is a file you write text to.
 *
 * The panel is 240x135 with a 6x8 font, i.e. 40 columns by 16 rows.  `echo
 * hello > /dev/lcd0` works too, and appends at the cursor. */

/* The user lib has printf but no snprintf, and the panel needs the text as
 * a buffer rather than as stdout.  This is the smallest formatter that
 * covers what the card prints: %d, %u and %s. */
static int printf_to(char *dst, int cap, const char *fmt, ...)
{
    __builtin_va_list ap;
    int n;

    __builtin_va_start(ap, fmt);
    n = tos_vsnprintf(dst, cap, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

static int lcd_open(void)
{
    int fd = open("/dev/lcd0");

    if (fd < 0) {
        puts("screen: this board has no display (or the driver did not bind)\n");
        puts("        see /proc/dev for what it does have\n");
    }
    return fd;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* "red", "0xf800", "63488".  The panel takes RGB565, so a name table is
 * worth more than a parser: nobody remembers that red is 0xf800. */
static int parse_color(const char *s, int *out)
{
    struct { const char *name; int rgb; } names[] = {
        { "black",   0x0000 }, { "white",  0xffff }, { "red",    0xf800 },
        { "green",   0x07e0 }, { "blue",   0x001f }, { "cyan",   0x07ff },
        { "yellow",  0xffe0 }, { "magenta",0xf81f }, { "gray",   0x8410 },
        { "grey",    0x8410 }, { "orange", 0xfd20 }, { "navy",   0x000f },
    };

    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (!strcmp(s, names[i].name)) {
            *out = names[i].rgb;
            return 0;
        }
    }

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        int v = 0;

        for (int i = 2; s[i]; i++) {
            int d = hex_digit(s[i]);
            if (d < 0)
                return -1;
            v = (v << 4) | d;
        }
        *out = v & 0xffff;
        return 0;
    }

    if (s[0] >= '0' && s[0] <= '9') {
        *out = atoi(s) & 0xffff;
        return 0;
    }
    return -1;
}

static int parse_on(const char *s, int *out)
{
    if (!strcmp(s, "on") || !strcmp(s, "1")) {
        *out = 1;
        return 0;
    }
    if (!strcmp(s, "off") || !strcmp(s, "0")) {
        *out = 0;
        return 0;
    }
    return -1;
}

/* The device table, drawn where the table is about the board it describes.
 * /proc/dev is a snapshot, so one read is the whole file. */
static int show_devices(int fd)
{
    /* The kernel draws this one: /proc/dev's columns are sized for a
     * shell and wrap into confetti at 40 columns, so the panel gets a
     * short form generated from the same table (LCD_OP_DEVS). */
    if (ioctl(fd, LCD_OP_DEVS, 0) < 0) {
        puts("screen: cannot draw the device map\n");
        return 1;
    }
    return 0;
}

static int status_card(int fd)
{
    int cols = ioctl(fd, LCD_OP_INFO, 0);
    int px = ioctl(fd, LCD_OP_PIXELS, 0);
    char line[64];
    int n;

    ioctl(fd, LCD_OP_CLEAR, 0x0000);
    ioctl(fd, LCD_OP_CURSOR, 0);
    ioctl(fd, LCD_OP_BACKLIGHT, 1);

    write(fd, "TinyOS\n", 7);

    n = printf_to(line, sizeof(line), "%dx%d px  %dx%d cells\n",
                  px >> 16, px & 0xffff, cols >> 16, cols & 0xffff);
    write(fd, line, n);

    n = printf_to(line, sizeof(line), "uptime %u s\n",
                  (unsigned)(uptime() / 1000));
    write(fd, line, n);

    write(fd, "\n", 1);
    write(fd, "screen dev   device map\n", 24);
    write(fd, "screen text  write text\n", 24);
    write(fd, "screen clear <color>\n", 21);
    write(fd, "screen mirror on|off\n", 21);
    write(fd, "screen invert on|off\n", 21);
    write(fd, "screen backlight on|off\n", 24);
    return 0;
}

static void usage(void)
{
    puts("usage: screen                    status card on the panel\n");
    puts("       screen dev                the board's device table\n");
    puts("       screen text <words...>    write a line\n");
    puts("       screen clear [color]      black, white, red, green, blue,\n");
    puts("                                 cyan, yellow, magenta, 0xRRGGBB\n");
    puts("       screen cursor <col> <row>\n");
    puts("       screen mirror on|off      mirror the console onto it\n");
    puts("       screen invert on|off\n");
    puts("       screen backlight on|off\n");
    puts("       screen info\n");
}

int main(int argc, char *argv[])
{
    int fd = lcd_open();

    if (fd < 0)
        return 1;

    if (argc < 2) {
        status_card(fd);
        close(fd);
        return 0;
    }

    if (!strcmp(argv[1], "dev")) {
        int rc = show_devices(fd);
        close(fd);
        return rc;
    }

    if (!strcmp(argv[1], "text")) {
        int rc = 0;

        for (int i = 2; i < argc; i++) {
            if (i > 2 && write(fd, " ", 1) < 0) {
                rc = 1;
                break;
            }
            if (write(fd, argv[i], (int)strlen(argv[i])) < 0) {
                rc = 1;
                break;
            }
        }
        if (write(fd, "\n", 1) < 0)
            rc = 1;
        close(fd);
        return rc;
    }

    if (!strcmp(argv[1], "clear")) {
        int color = 0x0000;
        if (argc > 2 && parse_color(argv[2], &color) < 0) {
            printf("screen: unknown colour '%s'\n", argv[2]);
            close(fd);
            return 2;
        }
        if (ioctl(fd, LCD_OP_CLEAR, color) < 0) {
            puts("screen: clear failed\n");
            close(fd);
            return 1;
        }
        close(fd);
        return 0;
    }

    if (!strcmp(argv[1], "cursor")) {
        int col, row, addr;

        if (argc < 4) {
            usage();
            close(fd);
            return 2;
        }
        col = atoi(argv[2]);
        row = atoi(argv[3]);
        addr = (row << 16) | (col & 0xffff);
        close(fd);
        fd = lcd_open();
        if (fd >= 0 && ioctl(fd, LCD_OP_CURSOR, addr) < 0) {
            puts("screen: cursor out of range\n");
            close(fd);
            return 1;
        }
        if (fd >= 0)
            close(fd);
        return 0;
    }

    if (!strcmp(argv[1], "mirror") || !strcmp(argv[1], "invert") ||
        !strcmp(argv[1], "backlight")) {
        int on, op;

        if (argc < 3 || parse_on(argv[2], &on) < 0) {
            printf("screen: %s needs on or off\n", argv[1]);
            close(fd);
            return 2;
        }
        op = !strcmp(argv[1], "mirror") ? LCD_OP_MIRROR :
             !strcmp(argv[1], "invert") ? LCD_OP_INVERT : LCD_OP_BACKLIGHT;

        if (ioctl(fd, op, on) < 0) {
            printf("screen: %s failed\n", argv[1]);
            close(fd);
            return 1;
        }
        printf("screen: %s %s\n", argv[1], on ? "on" : "off");
        close(fd);
        return 0;
    }

    if (!strcmp(argv[1], "info")) {
        int cols = ioctl(fd, LCD_OP_INFO, 0);
        int px = ioctl(fd, LCD_OP_PIXELS, 0);

        printf("/dev/lcd0: %dx%d pixels, %dx%d cells\n",
               px >> 16, px & 0xffff, cols >> 16, cols & 0xffff);
        printf("raw: info=0x%x pixels=0x%x\n",
               (unsigned)cols, (unsigned)px);
        close(fd);
        return 0;
    }

    if (!strcmp(argv[1], "help") || !strcmp(argv[1], "-h")) {
        usage();
        close(fd);
        return 0;
    }

    /* An unrecognised first word is treated as text: `screen hello` is what
     * people type first, and refusing it teaches nothing. */
    if (argv[1][0] != '-') {
        int rc = 0;

        for (int i = 1; i < argc; i++) {
            if (i > 1 && write(fd, " ", 1) < 0) {
                rc = 1;
                break;
            }
            if (write(fd, argv[i], (int)strlen(argv[i])) < 0) {
                rc = 1;
                break;
            }
        }
        if (write(fd, "\n", 1) < 0)
            rc = 1;
        close(fd);
        return rc;
    }

    /* Anything else: the first word was not a command, so it is text.
     * `screen hello` is what people type first; refusing it teaches
     * nothing. */
    {
        int rc = 0;

        for (int i = 1; i < argc; i++) {
            if (i > 1 && write(fd, " ", 1) < 0) {
                rc = 1;
                break;
            }
            if (write(fd, argv[i], (int)strlen(argv[i])) < 0) {
                rc = 1;
                break;
            }
        }
        if (write(fd, "\n", 1) < 0)
            rc = 1;
        close(fd);
        return rc;
    }
}
