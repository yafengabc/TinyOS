#include "tty.h"
#include "platform.h"
#include "console.h"
#include "string.h"

#define LINE_MAX 256

static char line[LINE_MAX];
static int  len;
static int  ready;

/* Cook one character: echo it, handle backspace, and mark the line
 * complete on newline.  Called from the tick, i.e. never from inside a
 * syscall, so a reader can block on "line ready" without racing us. */
static void tty_char(char c)
{
    /* Somebody has to collect the finished line before we start a new
       one.  One line of type-ahead, like a real terminal. */
    if (ready)
        return;

    if (c == '\r')
        c = '\n';

    if (c == '\n') {
        console_putc('\n');
        if (len < LINE_MAX)
            line[len++] = '\n';
        ready = 1;
        return;
    }

    if (c == 8 || c == 127) {
        if (len > 0) {
            len--;
            console_write("\b \b", 3);
        }
        return;
    }

    if (c >= 32 && c < 127 && len < LINE_MAX - 2) {
        line[len++] = c;
        console_putc(c);
    }
}

void tty_poll(void)
{
    /* Drain everything the device has; a burst arriving between two ticks
       must not be left behind for the next one. */
    for (int i = 0; i < LINE_MAX; i++) {
        int c = plat_console_getchar();
        if (c < 0)
            return;
        tty_char((char)c);
    }
}

int tty_line_ready(void)
{
    return ready;
}

int tty_take_line(char *dst, int max)
{
    int n = len < max ? len : max;

    if (n > 0)
        memcpy(dst, line, (size_t)n);

    len = 0;
    ready = 0;
    return n;
}
