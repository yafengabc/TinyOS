#include "lib.h"

/* fb - draw pixels on /dev/fb0, the panel's framebuffer view.
 *
 *   fb                a colour demo (gradient + checker + a disc)
 *   fb info           panel geometry
 *   fb bars           eight colour bars, a quick signal check
 *   fb clear [color]  fill the panel (name or 0xRRGGBB, default black)
 *   fb rect x y w h <color>   fill a rectangle
 *
 * write() takes RGB565 pixels, high byte first.  One write per scanline;
 * ioctl(FB_OP_RECT) selects where those rows land.  This is a real bitmap
 * path - no font, no character grid - so a program can push an image it
 * decoded itself. */

#define FB_MAX_W 256
static unsigned char row[FB_MAX_W * 2];

static int rgb565(int r, int g, int b)
{
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int named_color(const char *s)
{
    if (!strcmp(s, "black"))   return 0x0000;
    if (!strcmp(s, "white"))   return 0xFFFF;
    if (!strcmp(s, "red"))     return 0xF800;
    if (!strcmp(s, "green"))   return 0x07E0;
    if (!strcmp(s, "blue"))    return 0x001F;
    if (!strcmp(s, "cyan"))    return 0x07FF;
    if (!strcmp(s, "yellow"))  return 0xFFE0;
    if (!strcmp(s, "magenta")) return 0xF81F;
    return -1;
}

/* 0xRRGGBB -> RGB565, or a colour name. */
static int parse_color(const char *s)
{
    int n, v = 0, r, g, b;

    n = named_color(s);
    if (n >= 0)
        return n;
    if (s[0] != '0' || (s[1] != 'x' && s[1] != 'X'))
        return -1;
    s += 2;
    for (int i = 0; i < 6; i++) {
        int d = hex_digit(s[i]);
        if (d < 0 || s[i] == 0)
            return -1;
        v = (v << 4) | d;
    }
    r = (v >> 16) & 0xff;
    g = (v >> 8) & 0xff;
    b = v & 0xff;
    return rgb565(r, g, b);
}

static void put_px(int x, int color)
{
    row[x * 2]     = (unsigned char)(color >> 8);
    row[x * 2 + 1] = (unsigned char)color;
}

static void write_row(int fd, int w)
{
    if (write(fd, row, w * 2) < 0)
        puts("fb: scanline write failed\n");
}

static int fb_geometry(int fd, int *w, int *h)
{
    int v = ioctl(fd, FB_OP_INFO, 0);

    if (v <= 0)
        return -1;
    *w = v >> 16;
    *h = v & 0xffff;
    return (*w > 0 && *w <= FB_MAX_W && *h > 0) ? 0 : -1;
}

static int rect(int fd, int x, int y, int w, int h)
{
    if (ioctl(fd, FB_OP_RECT, FB_RECT(x, y, w, h)) < 0) {
        puts("fb: rectangle out of range\n");
        return -1;
    }
    return 0;
}

static int fill(int fd, int x, int y, int w, int h, int color)
{
    if (rect(fd, x, y, w, h) < 0)
        return -1;
    for (int i = 0; i < w; i++)
        put_px(i, color);
    for (int j = 0; j < h; j++)
        write_row(fd, w);
    ioctl(fd, FB_OP_SYNC, 0);
    return 0;
}

static void bars(int fd, int w, int h)
{
    static const int colors[8] = {
        0x0000, 0xF800, 0x07E0, 0x001F,
        0xFFE0, 0xF81F, 0x07FF, 0xFFFF,
    };

    if (rect(fd, 0, 0, w, h) < 0)
        return;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++)
            put_px(x, colors[(x * 8) / w]);
        write_row(fd, w);
    }
    ioctl(fd, FB_OP_SYNC, 0);
}

/* A real bitmap: a vertical hue ramp, a checker strip and a filled disc.
 * Built pixel by pixel, so this exercises the same path a decoded image
 * would use. */
static void demo(int fd, int w, int h)
{
    int cx = w / 2, cy = h / 2;
    int rad = h / 4;

    if (rect(fd, 0, 0, w, h) < 0)
        return;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int color;
            int r = (x * 255) / w;
            int g = (y * 255) / h;
            int b = 255 - r;

            color = rgb565(r, g, b);
            /* checker band across the middle */
            if (y > h / 2 - 8 && y < h / 2 + 8 &&
                ((x / 8 + y / 8) & 1))
                color = 0x0000;
            /* solid disc, deliberately not axis-aligned blocks */
            {
                int dx = x - cx, dy = y - cy;
                if (dx * dx + dy * dy < rad * rad)
                    color = 0xFFFF;
                if (dx * dx + dy * dy < (rad / 2) * (rad / 2))
                    color = 0xF800;
            }
            put_px(x, color);
        }
        write_row(fd, w);
    }
    ioctl(fd, FB_OP_SYNC, 0);
}

int main(int argc, char **argv)
{
    int fd = open("/dev/fb0");
    int w, h, color;

    if (fd < 0) {
        puts("fb: this board has no framebuffer (or the driver did not bind)\n");
        return 1;
    }
    if (fb_geometry(fd, &w, &h) < 0) {
        puts("fb: cannot read panel geometry\n");
        close(fd);
        return 1;
    }

    if (argc < 2 || !strcmp(argv[1], "demo")) {
        demo(fd, w, h);
        printf("fb: drew %dx%d demo bitmap\n", w, h);
    } else if (!strcmp(argv[1], "info")) {
        printf("fb0: %dx%d pixels, 16 bpp RGB565 (high byte first)\n", w, h);
        printf("     ioctl FB_OP_RECT x/y/w/h, then write() rows\n");
    } else if (!strcmp(argv[1], "bars")) {
        bars(fd, w, h);
        puts("fb: colour bars drawn\n");
    } else if (!strcmp(argv[1], "clear")) {
        color = 0x0000;
        if (argc > 2) {
            color = parse_color(argv[2]);
            if (color < 0) {
                puts("fb: bad colour (name or 0xRRGGBB)\n");
                close(fd);
                return 2;
            }
        }
        if (fill(fd, 0, 0, w, h, color) < 0) {
            close(fd);
            return 1;
        }
        puts("fb: panel cleared\n");
    } else if (!strcmp(argv[1], "rect")) {
        int x, y, rw, rh;
        if (argc < 7) {
            puts("usage: fb rect x y w h <color>\n");
            close(fd);
            return 2;
        }
        x = atoi(argv[2]); y = atoi(argv[3]);
        rw = atoi(argv[4]); rh = atoi(argv[5]);
        color = parse_color(argv[6]);
        if (rw <= 0 || rh <= 0 || color < 0) {
            puts("fb: bad rectangle or colour\n");
            close(fd);
            return 2;
        }
        if (fill(fd, x, y, rw, rh, color) < 0) {
            close(fd);
            return 1;
        }
        printf("fb: filled %dx%d at (%d,%d)\n", rw, rh, x, y);
    } else {
        puts("usage: fb [demo|info|bars|clear [color]|rect x y w h color]\n");
        close(fd);
        return 2;
    }

    close(fd);
    return 0;
}
