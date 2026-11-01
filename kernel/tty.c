#include "tty.h"
#include "platform.h"
#include "console.h"
#include "process.h"
#include "string.h"
#include "../include/tos.h"

/* Cooked input, kept as a queue of finished lines.  A burst - a paste, or a
 * script feeding several commands - arrives in one tty_poll() and must not
 * lose the lines after the first: the shell reads them one at a time and
 * answers each.  The queue holds whole lines only; a half-typed line has
 * its own buffer and joins the queue on the newline. */
#define TTY_Q_LINES 8
#define TTY_LINE_MAX 256

static char q[TTY_Q_LINES][TTY_LINE_MAX];
static int  q_len[TTY_Q_LINES];
static int  q_head, q_tail;            /* ring: tail == head means empty   */

static char pending[TTY_LINE_MAX];     /* the line being typed             */
static int  pending_len;

/* Control characters seen since the last tick.  They are not part of the
 * line: ^C in the middle of a half-typed command throws the command away,
 * like any terminal line discipline.  Two bits per key, because a burst can
 * carry ^Z ... ^Z (stop, then continue) and the order matters. */
#define CTRL_STOP 1
#define CTRL_CONT 2
static int ctrl_c, ctrl_z, ctrl_bs;

/* Cook one character: echo it, handle backspace, and publish the line on a
 * newline.  Called from the tick, i.e. never from inside a syscall, so a
 * reader can block on "line ready" without racing us.  Echo goes through
 * console_echo(), which makes room for itself in the output. */
static void tty_char(char c)
{
    if (c == 3 || c == 26 || c == 28) {
        /* ^C kills, ^Z toggles stop/continue, ^\ kills without a core
           dump (there are no core dumps).  The half-typed line is dropped
           so that the next prompt starts clean. */
        if (c == 3)
            ctrl_c = 1;
        if (c == 26)
            ctrl_z = (ctrl_z & CTRL_STOP) ? ((ctrl_z & ~CTRL_STOP) | CTRL_CONT) : CTRL_STOP;
        if (c == 28)
            ctrl_bs = 1;
        pending_len = 0;
        console_echo("^C\n", 3);
        return;
    }

    if (c == '\r')
        c = '\n';

    if (c == '\n') {
        console_echo("\n", 1);
        /* A full queue drops the line - the reader is too far behind for
           more type-ahead to mean anything. */
        int next = (q_tail + 1) % TTY_Q_LINES;
        if (next != q_head) {
            int n = pending_len < TTY_LINE_MAX - 1 ? pending_len : TTY_LINE_MAX - 1;
            memcpy(q[q_tail], pending, (size_t)n);
            q[q_tail][n] = '\0';
            q_len[q_tail] = n + 1;
            q_tail = next;
        }
        pending_len = 0;
        return;
    }

    if (c == 8 || c == 127) {
        if (pending_len > 0) {
            pending_len--;
            console_echo("\b \b", 3);
        }
        return;
    }

    if (c >= 32 && c < 127 && pending_len < TTY_LINE_MAX - 2) {
        pending[pending_len++] = c;
        console_echo(&c, 1);
    }
}

void tty_poll(void)
{
    /* Drain everything the device has: a burst arriving between two ticks
       must be cooked in the same tick, or the echo and the answers drift
       apart. */
    for (int i = 0; i < 4 * TTY_LINE_MAX; i++) {
        int c = plat_console_getchar();
        if (c < 0)
            return;
        tty_char((char)c);
    }
}

int tty_line_ready(void)
{
    return q_head != q_tail;
}

int tty_take_line(char *dst, int max)
{
    if (q_head == q_tail)
        return 0;

    int n = q_len[q_head] < max ? q_len[q_head] : max;
    if (n > 0)
        memcpy(dst, q[q_head], (size_t)n);
    q_head = (q_head + 1) % TTY_Q_LINES;
    return n;
}

void tty_ctrl_deliver(void)
{
    /* Nobody at the console (an idle tick, or only background tasks): the
       keys had no target either, so drop them. */
    int pid = process_console_reader();

    if (ctrl_bs || ctrl_c) {
        if (pid > 0)
            (void)process_signal(pid, SIGKILL);
        ctrl_c = ctrl_bs = 0;
    }
    if (ctrl_z) {
        if (pid > 0) {
            if (ctrl_z & CTRL_CONT)
                (void)process_signal(pid, SIGCONT);
            else
                (void)process_signal(pid, SIGSTOP);
        }
        ctrl_z = 0;
    }
}
