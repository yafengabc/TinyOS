#include "hwdev.h"
#include "platform.h"
#include "devfs.h"
#include "console.h"
#include "string.h"

/* The device classes, with the board behind them.
 *
 * Nothing here knows a part number.  A board reports "led0, GPIO3, active
 * low" (board/rp2350/board_devs.c), this file turns that into /dev/led0,
 * and a board that reports nothing gets no nodes and -1 when a program
 * asks.  The hooks are weak for exactly that reason: a board implements
 * only the classes it has. */

/* The panel-side renderers live next to the table they draw, further down;
 * the ioctl that triggers them is above. */
static const char *class_short(uint8_t cls);
static void draw_devs(void);

/* ---- weak defaults ---------------------------------------------------- */

__attribute__((weak)) const board_dev_t *plat_board_devs(int *count)
{
    if (count)
        *count = 0;
    return (const board_dev_t *)0;
}

__attribute__((weak)) const char *plat_board_name(void)
{
    return "unknown";
}

__attribute__((weak)) int plat_led_count(void) { return 0; }
__attribute__((weak)) int plat_led_set(int idx, int on)
{
    (void)idx; (void)on;
    return -1;
}
__attribute__((weak)) int plat_led_get(int idx) { (void)idx; return -1; }

__attribute__((weak)) int plat_key_count(void) { return 0; }
__attribute__((weak)) int plat_key_read(int idx) { (void)idx; return -1; }

__attribute__((weak)) int plat_display_count(void) { return 0; }
__attribute__((weak)) int plat_display_info(int idx, int *cols, int *rows,
                                            int *cw, int *ch)
{
    (void)idx; (void)cols; (void)rows; (void)cw; (void)ch;
    return -1;
}
__attribute__((weak)) int plat_display_cell(int idx, int col, int row, char c,
                                            uint16_t fg, uint16_t bg)
{
    (void)idx; (void)col; (void)row; (void)c; (void)fg; (void)bg;
    return -1;
}
__attribute__((weak)) int plat_display_row(int idx, int row,
                                           const char *chars, int ncols,
                                           uint16_t fg, uint16_t bg)
{
    (void)idx; (void)row; (void)chars; (void)ncols; (void)fg; (void)bg;
    return -1;
}
__attribute__((weak)) void plat_display_sync(int idx)
{
    (void)idx;
}
__attribute__((weak)) int plat_display_scan(int idx, int x, int y, int w,
                                             const uint8_t *px)
{
    (void)idx; (void)x; (void)y; (void)w; (void)px;
    return -1;
}
__attribute__((weak)) int plat_display_clear(int idx, uint16_t color)
{
    (void)idx; (void)color;
    return -1;
}
__attribute__((weak)) int plat_display_backlight(int idx, int on)
{
    (void)idx; (void)on;
    return -1;
}
__attribute__((weak)) int plat_display_backlight_get(int idx)
{
    (void)idx;
    return -1;
}

/* ---- text parsers ----------------------------------------------------- */

static int skip_space(const char *s, uint32_t len, uint32_t *i)
{
    while (*i < len && (s[*i] == ' ' || s[*i] == '\t' ||
                        s[*i] == '\n' || s[*i] == '\r'))
        (*i)++;
    return *i < len;
}

/* "1", "0", "on", "off", "true", "false".  A device file is written by
 * `echo` as often as by a program, so both spellings have to work. */
static int parse_bool(const char *s, uint32_t len, int *out)
{
    uint32_t i = 0;

    if (!skip_space(s, len, &i))
        return -1;
    if (s[i] == '1') {
        *out = 1;
        return 0;
    }
    if (s[i] == '0') {
        *out = 0;
        return 0;
    }
    if (len - i >= 2 && s[i] == 'o' && s[i + 1] == 'n') {
        *out = 1;
        return 0;
    }
    if (len - i >= 3 && s[i] == 'o' && s[i + 1] == 'f' && s[i + 2] == 'f') {
        *out = 0;
        return 0;
    }
    if (len - i >= 4 && s[i] == 't' && s[i + 1] == 'r') {
        *out = 1;
        return 0;
    }
    if (len - i >= 5 && s[i] == 'f' && s[i + 1] == 'a') {
        *out = 0;
        return 0;
    }
    return -1;
}

/* ---- LED -------------------------------------------------------------- */

#define MAX_CLASS_DEVS 8
static int led_idx[MAX_CLASS_DEVS];
static int key_idx[MAX_CLASS_DEVS];
static int lcd_idx[MAX_CLASS_DEVS];

static int led_dev_read(void *priv, uint32_t off, void *buf, uint32_t len)
{
    char tmp[4];
    int v, n;

    if (off)
        return 0;
    v = plat_led_get(*(int *)priv);
    if (v < 0)
        return -1;
    n = ksnprintf(tmp, sizeof(tmp), "%d\n", v ? 1 : 0);
    if (n < 0)
        return -1;
    if ((uint32_t)n > len)
        n = (int)len;
    memcpy(buf, tmp, (size_t)n);
    return n;
}

static int led_dev_write(void *priv, uint32_t off, const void *buf,
                         uint32_t len)
{
    int on;

    (void)off;
    if (parse_bool((const char *)buf, len, &on) < 0)
        return -1;
    if (plat_led_set(*(int *)priv, on) < 0)
        return -1;
    return (int)len;
}

static int led_dev_ioctl(void *priv, int cmd, int arg)
{
    switch (cmd) {
    case 0:  return plat_led_set(*(int *)priv, arg != 0);
    case 1:  return plat_led_get(*(int *)priv);
    default: return -1;
    }
}

static const struct dev_ops led_ops = {
    .read  = led_dev_read,
    .write = led_dev_write,
    .ioctl = led_dev_ioctl,
};

/* ---- key -------------------------------------------------------------- */

static int key_dev_read(void *priv, uint32_t off, void *buf, uint32_t len)
{
    char tmp[4];
    int v, n;

    if (off)
        return 0;
    v = plat_key_read(*(int *)priv);
    if (v < 0)
        return -1;
    n = ksnprintf(tmp, sizeof(tmp), "%d\n", v ? 1 : 0);
    if (n < 0)
        return -1;
    if ((uint32_t)n > len)
        n = (int)len;
    memcpy(buf, tmp, (size_t)n);
    return n;
}

static int key_dev_write(void *priv, uint32_t off, const void *buf,
                         uint32_t len)
{
    (void)priv; (void)off; (void)buf; (void)len;
    return -1;
}

static int key_dev_ioctl(void *priv, int cmd, int arg)
{
    (void)arg;
    switch (cmd) {
    case 0:  return plat_key_read(*(int *)priv);
    default: return -1;
    }
}

static const struct dev_ops key_ops = {
    .read  = key_dev_read,
    .write = key_dev_write,
    .ioctl = key_dev_ioctl,
};

/* ---- display: a character device that draws text ---------------------- */

static int lcd_dev_read(void *priv, uint32_t off, void *buf, uint32_t len)
{
    (void)priv; (void)off; (void)buf; (void)len;
    return -1;              /* a screen is write-only from here */
}

static int lcd_dev_write(void *priv, uint32_t off, const void *buf,
                         uint32_t len)
{
    const char *s = (const char *)buf;

    (void)off;
    if (*(int *)priv != 0)
        return -1;          /* only the first panel carries the console */
    for (uint32_t i = 0; i < len; i++)
        screen_putc(s[i]);
    return (int)len;
}

static int lcd_dev_ioctl(void *priv, int cmd, int arg)
{
    int w, h, cw, ch, prev;

    (void)priv;

    switch (cmd) {
    case LCD_OP_INFO:
        return (screen_cols() << 16) | (screen_rows() & 0xffff);
    case LCD_OP_CLEAR:
        screen_clear((uint16_t)arg);
        return 0;
    case LCD_OP_CURSOR:
        screen_cursor(arg & 0xffff, (arg >> 16) & 0xffff);
        return 0;
    case LCD_OP_BACKLIGHT:
        prev = plat_display_backlight_get(0);
        if (plat_display_backlight(0, arg != 0) < 0)
            return -1;
        return prev < 0 ? 0 : prev;
    case LCD_OP_MIRROR:
        prev = screen_mirror_on();
        screen_mirror(arg != 0);
        return prev;
    case LCD_OP_MIRROR_GET:
        return screen_mirror_on();
    case LCD_OP_INVERT:
        prev = screen_inverted();
        screen_invert(arg != 0);
        return prev;
    case LCD_OP_FG:
        prev = (int)screen_fg();
        screen_colors((uint16_t)arg, screen_bg());
        return prev;
    case LCD_OP_BG:
        prev = (int)screen_bg();
        screen_colors(screen_fg(), (uint16_t)arg);
        return prev;
    case LCD_OP_PIXELS:
        if (plat_display_info(0, &w, &h, &cw, &ch) < 0)
            return -1;
        return (w << 16) | (h & 0xffff);
    case LCD_OP_DEVS:
        if (screen_cols() <= 0)
            return -1;
        draw_devs();
        return 0;
    default:
        return -1;
    }
}

static const struct dev_ops lcd_ops = {
    .read  = lcd_dev_read,
    .write = lcd_dev_write,
    .ioctl = lcd_dev_ioctl,
};

/* ---- display: a pixel framebuffer, /dev/fb0 --------------------------- */

/* The rectangle selected by FB_OP_RECT is per open descriptor in spirit,
 * but there is one panel and one console, so a single shared cursor is
 * enough and keeps the driver allocation-free. */
#define FB_MAX_W 256

static int     fb_idx[MAX_CLASS_DEVS];
static int     fb_rx, fb_ry, fb_w, fb_h;
static int     fb_row;                 /* next scanline inside the rect  */
static uint8_t fb_line[FB_MAX_W * 2];

static int fb_dev_read(void *priv, uint32_t off, void *buf, uint32_t len)
{
    (void)priv; (void)off; (void)buf; (void)len;
    return -1;              /* a framebuffer is write-only from here */
}

static int fb_dev_write(void *priv, uint32_t off, const void *buf,
                        uint32_t len)
{
    const uint8_t *src = (const uint8_t *)buf;
    int idx = *(int *)priv;
    uint32_t used = 0;
    int stride = fb_w * 2;

    (void)off;
    if (idx != 0 || fb_w <= 0 || fb_h <= 0)
        return -1;

    while (fb_row < fb_h && used + (uint32_t)stride <= len) {
        /* Quiesce BEFORE refilling: the previous scanline's DMA may still
         * be reading fb_line.  Copy out of the user buffer after that, so
         * the caller can reuse its buffer as soon as write() returns. */
        plat_display_sync(idx);
        memcpy(fb_line, src + used, (size_t)stride);
        if (plat_display_scan(idx, fb_rx, fb_ry + fb_row, fb_w, fb_line) < 0)
            return -1;
        used += (uint32_t)stride;
        fb_row++;
    }
    return (int)used;
}

static int fb_dev_ioctl(void *priv, int cmd, int arg)
{
    int idx = *(int *)priv;
    int w, h, cw, ch;
    int x, y, rw, rh;

    if (idx != 0)
        return -1;

    switch (cmd) {
    case FB_OP_INFO:
        if (plat_display_info(0, &w, &h, &cw, &ch) < 0)
            return -1;
        return (w << 16) | (h & 0xffff);
    case FB_OP_RECT:
        x = FB_RECT_X(arg); y = FB_RECT_Y(arg);
        rw = FB_RECT_W(arg); rh = FB_RECT_H(arg);
        if (plat_display_info(0, &w, &h, &cw, &ch) < 0)
            return -1;
        if (rw <= 0 || rh <= 0 || rw > FB_MAX_W ||
            x < 0 || y < 0 || x + rw > w || y + rh > h)
            return -1;
        fb_rx = x; fb_ry = y; fb_w = rw; fb_h = rh; fb_row = 0;
        return 0;
    case FB_OP_SYNC:
        plat_display_sync(0);
        return 0;
    case FB_OP_CLEAR:
        fb_row = fb_h;      /* a clear invalidates any pending rectangle */
        return plat_display_clear(0, (uint16_t)arg);
    default:
        return -1;
    }
}

static const struct dev_ops fb_ops = {
    .read  = fb_dev_read,
    .write = fb_dev_write,
    .ioctl = fb_dev_ioctl,
};

/* ---- /proc/dev: the board's own description --------------------------- */

/* Left-justified append: text first, then spaces up to `width`.  A device
 * list that does not line up is not a table, and the kernel formatter has
 * no "%-8s". */
static void append_padded(char *dst, uint32_t cap, const char *s, int width)
{
    uint32_t n = (uint32_t)strlen(dst);
    int len = 0;

    for (int i = 0; s[i] && n + 1 < cap; i++) {
        dst[n++] = s[i];
        len++;
    }
    while (len < width && n + 1 < cap) {
        dst[n++] = ' ';
        len++;
    }
    dst[n] = '\0';
}

static const char *class_name(uint8_t cls)
{
    switch (cls) {
    case TOS_DEV_LED:     return "led";
    case TOS_DEV_KEY:     return "key";
    case TOS_DEV_DISPLAY: return "display";
    case TOS_DEV_STORAGE: return "storage";
    case TOS_DEV_UART:    return "uart";
    case TOS_DEV_USB:     return "usb";
    case TOS_DEV_ADC:     return "adc";
    case TOS_DEV_FLASH:   return "flash";
    case TOS_DEV_GPIO:    return "gpio";
    case TOS_DEV_SENSOR:  return "sensor";
    default:              return "?";
    }
}

/* Five letters or fewer: the panel has 40 columns and eleven devices to
 * fit, so the long names exist only in /proc/dev. */
static const char *class_short(uint8_t cls)
{
    switch (cls) {
    case TOS_DEV_LED:     return "led";
    case TOS_DEV_KEY:     return "key";
    case TOS_DEV_DISPLAY: return "disp";
    case TOS_DEV_STORAGE: return "store";
    case TOS_DEV_UART:    return "uart";
    case TOS_DEV_USB:     return "usb";
    case TOS_DEV_ADC:     return "adc";
    case TOS_DEV_FLASH:   return "flash";
    case TOS_DEV_GPIO:    return "gpio";
    case TOS_DEV_SENSOR:  return "sensor";
    default:              return "?";
    }
}

static int dev_list_read(void *priv, uint32_t off, void *buf, uint32_t len)
{
    /* The table is rebuilt per read and served as a slice: a device list
     * outgrew one buffer the moment the board had a dozen entries, and a
     * `cat` that stops in the middle of a line is worse than no file.  The
     * text fits in TABLE_BYTES; if a future board does not, it is cut at a
     * line boundary and the last line says so. */
    enum { TABLE_BYTES = 1024 };
    char out[TABLE_BYTES];
    char line[128];
    int n, count = 0;
    uint32_t used = 0;
    const board_dev_t *devs;

    (void)priv;

    devs = plat_board_devs(&count);
    n = ksnprintf(out, sizeof(out), "board: %s\n", plat_board_name());
    used = (uint32_t)(n > 0 ? n : 0);

    for (int i = 0; i < count && used + 1 < sizeof(out); i++) {
        const board_dev_t *d = &devs[i];
        uint32_t n2;

        line[0] = '\0';
        append_padded(line, sizeof(line), d->name, 9);
        append_padded(line, sizeof(line), class_name(d->cls), 9);
        if (d->pin >= 0) {
            char pinbuf[12];
            ksnprintf(pinbuf, sizeof(pinbuf), "pin %d", d->pin);
            append_padded(line, sizeof(line), pinbuf, 8);
        } else {
            append_padded(line, sizeof(line), "-", 8);
        }
        append_padded(line, sizeof(line), d->detail ? d->detail : "", 0);
        if (d->model && d->model[0]) {
            n2 = (uint32_t)strlen(line);
            ksnprintf(line + n2, sizeof(line) - n2, " [%s]", d->model);
        }
        if (d->flags & TOS_DEV_SHARED) {
            n2 = (uint32_t)strlen(line);
            ksnprintf(line + n2, sizeof(line) - n2, " (shared)");
        }

        n = ksnprintf(out + used, sizeof(out) - used, "%s\n", line);
        if (n < 0)
            break;
        used += (uint32_t)n;
    }

    if (off >= used)
        return 0;
    if (used - off < len)
        len = used - off;
    memcpy(buf, out + off, len);
    return (int)len;
}

/* The same table, laid out for a 40-column panel.
 *
 * A panel is not a terminal: /proc/dev's columns are sized for a shell and
 * wrap into confetti on a 1.14" screen.  So the short form is generated
 * here, next to the data, instead of being re-derived by a user program
 * that would have to parse the long one.  Detail is truncated, not
 * reflowed - one device per row is the whole point. */
static void draw_devs(void)
{
    const board_dev_t *devs;
    int count = 0, cols = screen_cols();
    char line[64];

    if (cols <= 0)
        return;

    devs = plat_board_devs(&count);

    screen_clear(0x0000);
    screen_cursor(0, 0);
    screen_puts("board: ");
    screen_puts(plat_board_name());
    screen_putc('\n');

    for (int i = 0; i < count; i++) {
        const board_dev_t *d = &devs[i];
        uint32_t n = 0;
        int room;

        line[0] = '\0';
        append_padded(line, sizeof(line), d->name, 7);
        append_padded(line, sizeof(line), class_short(d->cls), 6);
        if (d->pin >= 0) {
            char pinbuf[12];
            ksnprintf(pinbuf, sizeof(pinbuf), "pin %d", d->pin);
            append_padded(line, sizeof(line), pinbuf, 7);
        } else {
            append_padded(line, sizeof(line), "-", 7);
        }

        /* Whatever is left of the row goes to the wiring note. */
        n = (uint32_t)strlen(line);
        room = cols - (int)n - 1;
        if (room > 4 && d->detail && d->detail[0]) {
            for (int k = 0; d->detail[k] && k < room; k++)
                line[n + (uint32_t)k] = d->detail[k];
            n += (uint32_t)(room < (int)strlen(d->detail) ? room
                                                          : strlen(d->detail));
            line[n] = '\0';
        }

        screen_puts(line);
        screen_putc('\n');
    }
}

static const struct dev_ops dev_list_ops = {
    .read = dev_list_read,
};

/* ---- registration ----------------------------------------------------- */

static int g_init_done;

void hwdev_init(void)
{
    const board_dev_t *devs;
    int count = 0, nl = 0, nk = 0, nd = 0;
    char name[16];

    if (g_init_done)
        return;
    g_init_done = 1;

    devs = plat_board_devs(&count);

    for (int i = 0; i < count; i++) {
        switch (devs[i].cls) {
        case TOS_DEV_LED:
            if (nl >= MAX_CLASS_DEVS)
                break;
            led_idx[nl] = nl;
            ksnprintf(name, sizeof(name), "dev/%s", devs[i].name);
            devfs_register(name, &led_ops, &led_idx[nl]);
            nl++;
            break;
        case TOS_DEV_KEY:
            if (nk >= MAX_CLASS_DEVS)
                break;
            key_idx[nk] = nk;
            ksnprintf(name, sizeof(name), "dev/%s", devs[i].name);
            devfs_register(name, &key_ops, &key_idx[nk]);
            nk++;
            break;
        case TOS_DEV_DISPLAY:
            if (nd >= MAX_CLASS_DEVS)
                break;
            lcd_idx[nd] = nd;
            ksnprintf(name, sizeof(name), "dev/lcd%d", nd);
            devfs_register(name, &lcd_ops, &lcd_idx[nd]);
            if (nd == 0) {
                devfs_register("dev/screen", &lcd_ops, &lcd_idx[0]);
                /* Same panel, pixel view.  Only the first display gets a
                 * framebuffer: one console, one bitmap surface. */
                fb_idx[0] = 0;
                devfs_register("dev/fb0", &fb_ops, &fb_idx[0]);
            }
            nd++;
            break;
        default:
            break;          /* described in /proc/dev, no node of its own */
        }
    }

    devfs_register("proc/dev", &dev_list_ops, NULL);

    /* A full device table is silent by nature - the node simply never
     * appears - so say it once at boot, where someone will see it. */
    if (devfs_dropped() > 0) {
        char msg[72];
        int n = ksnprintf(msg, sizeof(msg),
                          "devfs: table full (MAX_DEVS), %d device(s) missing\n",
                          devfs_dropped());
        console_write(msg, (size_t)n);
    }
}
