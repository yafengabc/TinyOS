#include "console.h"
#include "platform.h"
#include "string.h"

/* Architecture independent console: formatting + routing to the platform's
 * character output.  The VGA text buffer, UART or any other device lives
 * behind plat_console_putc(). */

static void out_char(char c)
{
    if (c == '\n')
        plat_console_putc('\r');
    plat_console_putc(c);
}

struct outbuf {
    char *buf;
    size_t cap;
    size_t len;
};

static void ob_putc(struct outbuf *o, char c)
{
    if (o->len + 1 < o->cap)
        o->buf[o->len] = c;
    o->len++;
}

static void ob_str(struct outbuf *o, const char *s)
{
    while (*s)
        ob_putc(o, *s++);
}

static void ob_uint(struct outbuf *o, uint32_t v, uint32_t base, int width, char pad)
{
    char tmp[16];
    int k = 0;
    if (v == 0)
        tmp[k++] = '0';
    while (v) {
        uint32_t d = v % base;
        tmp[k++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    }
    while (width-- > k)
        ob_putc(o, pad);
    while (k--)
        ob_putc(o, tmp[k]);
}

static void ob_int(struct outbuf *o, int32_t v, int width, char pad)
{
    uint32_t u;
    if (v < 0) {
        ob_putc(o, '-');
        u = (uint32_t)(-(int64_t)v);
    } else {
        u = (uint32_t)v;
    }
    ob_uint(o, u, 10, width, pad);
}

int kvsnprintf(char *buf, size_t size, const char *fmt, __builtin_va_list ap)
{
    struct outbuf o = { buf, size, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            ob_putc(&o, *fmt);
            continue;
        }
        fmt++;

        int width = 0;
        char pad = ' ';
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        switch (*fmt) {
        case 'd':
        case 'i':
            ob_int(&o, __builtin_va_arg(ap, int), width, pad);
            break;
        case 'u':
            ob_uint(&o, __builtin_va_arg(ap, unsigned int), 10, width, pad);
            break;
        case 'x':
            ob_uint(&o, __builtin_va_arg(ap, unsigned int), 16, width, pad);
            break;
        case 'c':
            ob_putc(&o, (char)__builtin_va_arg(ap, int));
            break;
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            ob_str(&o, s);
            break;
        }
        case '%':
            ob_putc(&o, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            ob_putc(&o, '%');
            ob_putc(&o, *fmt);
            break;
        }
    }

    if (size)
        buf[o.len < size ? o.len : size - 1] = '\0';
    return (int)o.len;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

void kprintf(const char *fmt, ...)
{
    char buf[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);

    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    for (int i = 0; i < n; i++)
        out_char(buf[i]);
}

void console_init(void)
{
    plat_console_init();
}

void console_clear(void)
{
    /* The platform may offer a clear; otherwise newlines are enough. */
}

void console_set_color(uint8_t fg, uint8_t bg)
{
    (void)fg;
    (void)bg;
}

void console_putc(char c)
{
    out_char(c);
}

void console_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++)
        out_char(s[i]);
}

void panic(const char *fmt, ...)
{
    kprintf("\n*** KERNEL PANIC ***\n");
    char buf[256];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = kvsnprintf(buf, sizeof(buf), fmt, ap);
    __builtin_va_end(ap);
    if (n > (int)sizeof(buf) - 1)
        n = sizeof(buf) - 1;
    for (int i = 0; i < n; i++)
        out_char(buf[i]);
    kprintf("\nSystem halted.\n");
    plat_panic_halt();
}
