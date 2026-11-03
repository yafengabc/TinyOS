#include "tty.h"
#include "platform.h"
#include "console.h"
#include "process.h"
#include "string.h"
#include "../include/tos.h"

/* Cooked queue: a 1 KiB byte pool holding finished lines, each stored as a
 * two-byte length followed by the bytes (NUL included), so a line costs
 * exactly what it uses rather than a whole 256-byte slot.  A burst - a
 * paste, or a script feeding several commands - arrives in one tty_poll()
 * and must not lose the lines after the first: the shell reads them one at
 * a time and answers each.
 *
 * This is type-ahead, not rate.  With fixed slots, eight 256-byte lines
 * meant "up to eight commands, however short"; the pool means "up to 1 KiB
 * of text", which is ~120 six-byte commands or three maximum-length ones.
 * The trade is deliberate: short commands are what a paste carries.
 *
 * Both consoles land their bytes from real receive interrupts (the C3
 * routes UART0 and the USB-Serial/JTAG packet interrupt each to its own
 * CPU line), so a burst is absorbed the instant it arrives instead of one
 * tick's worth.  The x86 and qemu ports keep polling from the tick and
 * just see a deeper type-ahead. */
#define TTY_Q_BYTES  1024
#define TTY_LINE_MAX 256

static char q_pool[TTY_Q_BYTES];
static int  q_base;                    /* offset of the oldest line's header */
static int  q_fill;                    /* bytes in use from q_base           */
static int  q_cnt;                     /* lines queued                       */

static char pending[TTY_LINE_MAX];     /* the line being typed             */
static int  pending_len;

/* Queue one finished line of n bytes (NUL included).  The pool is linear:
 * the read end only advances, so when the tail runs out of room the live
 * bytes are slid back to the front before the line is dropped.  Losing a
 * line is the last resort - a reader that far behind would not act on more
 * type-ahead anyway. */
static void q_push(const char *s, int n)
{
    /* Both the used bytes and where the tail sits matter: the oldest line
     * may have moved far into the pool, so "q_fill + n fits in the pool"
     * is not the same as "it fits after q_base". */
    if (q_base + q_fill + 2 + n > TTY_Q_BYTES) {
        memmove(q_pool, q_pool + q_base, (size_t)q_fill);
        q_base = 0;
        if (q_fill + 2 + n > TTY_Q_BYTES)
            return;
    }
    int off = q_base + q_fill;
    q_pool[off] = (char)(n & 0xFF);
    q_pool[off + 1] = (char)((n >> 8) & 0xFF);
    memcpy(q_pool + off + 2, s, (size_t)n);
    q_fill += 2 + n;
    q_cnt++;
}

/* Control characters seen since the last tick.  They are not part of the
 * line: ^C in the middle of a half-typed command throws the command away,
 * like any terminal line discipline.  Two bits per key, because a burst can
 * carry ^Z ... ^Z (stop, then continue) and the order matters. */
#define CTRL_STOP 1
#define CTRL_CONT 2
static int ctrl_c, ctrl_z, ctrl_bs;
static int ctrl_age;               /* ticks a pending ^C has waited */
static int cr_seen;                /* the previous cooked byte was a CR */

/* ---- raw mode --------------------------------------------------------
 * A full-screen program needs every keystroke, including the ESC and the
 * two bytes after it that make up a cursor-key sequence.  The cooked path
 * below eats all of that: only 32..126 reach the line buffer and nothing is
 * delivered before a newline, so an editor would never see an arrow key.
 *
 * Raw mode replaces the line buffer with a byte queue, one read() at a time.
 * Signals still work (^C has to be able to kill a wedged editor); the mode
 * belongs to the console, not to the process, and process.c drops it when
 * the reading task dies so the shell gets its line discipline back.
 *
 * How deep the queue has to be: the receive interrupt hands bytes straight
 * to a blocked reader (process_console_kick), so the queue only fills while
 * that reader is waiting to be scheduled - at worst one QUANTUM, 5 ticks =
 * 50 ms (process.c).  A raw reader asks for one byte per read(), so every
 * byte costs it a wake-up; the wire meanwhile delivers 576 bytes in those
 * 50 ms at 115200.  1 KiB covers that with margin, and anything bigger is
 * memory that no achievable baud rate and quantum can use. */
#define RAW_Q 1024
static char raw_q[RAW_Q];
static int  raw_head, raw_tail;        /* ring: head == tail means empty */
static int  raw_on;

void tty_set_raw(int on)
{
    cr_seen = 0;
    if (on) {
        /* Drop a half-typed command: it belongs to the shell, and the
         * editor's first keystroke must not complete it. */
        pending_len = 0;
    } else {
        raw_head = raw_tail = 0;
    }
    raw_on = on ? 1 : 0;
}

int tty_is_raw(void)
{
    return raw_on;
}

int tty_raw_ready(void)
{
    return raw_head != raw_tail;
}

int tty_take_raw(char *dst, int max)
{
    int n = 0;
    while (n < max && raw_head != raw_tail) {
        dst[n++] = raw_q[raw_head];
        raw_head = (raw_head + 1) % RAW_Q;
    }
    return n;
}

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
        cr_seen = 0;
        console_echo("^C\n", 3);
        return;
    }

    if (raw_on) {
        /* Byte for byte: no echo (the program draws its own screen), no
         * backspace handling, no line assembly, and no CR to LF either -
         * the editor wants to see what the terminal actually sent.  A full
         * queue drops the newest byte, which is the same trade the echo
         * queue makes. */
        int next = (raw_tail + 1) % RAW_Q;
        if (next != raw_head) {
            raw_q[raw_tail] = c;
            raw_tail = next;
        }
        return;
    }

    /* CR and LF both end a line, but a terminal or a paste usually sends
     * them as a pair.  Without swallowing the second half, every pasted
     * line publishes an empty line as well: the shell answers each with a
     * bare prompt, and the empty lines take queue slots away from the
     * commands behind them (measured: a 12-line CRLF paste ran 5 commands
     * where the same paste with LF alone ran 9). */
    if (c == '\r') {
        cr_seen = 1;
        c = '\n';
    } else if (c == '\n' && cr_seen) {
        cr_seen = 0;
        return;
    } else {
        cr_seen = 0;
    }

    if (c == '\n') {
        int n = pending_len < TTY_LINE_MAX - 1 ? pending_len : TTY_LINE_MAX - 1;
        console_echo("\n", 1);
        pending[n] = '\0';
        q_push(pending, n + 1);
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
    /* Drain everything the device has: a burst arriving between two
       drains must be cooked in the same drain, or the echo and the
       answers drift apart.  The bound only guards against a device that
       never runs dry - a full raw queue is the most that can be waiting
       on top of it, so twice that is safely above anything reachable. */
    for (int i = 0; i < 2 * RAW_Q; i++) {
        int c = plat_console_getchar();
        if (c < 0)
            return;
        tty_char((char)c);
    }
}

int tty_line_ready(void)
{
    return q_cnt > 0;
}

int tty_take_line(char *dst, int max)
{
    if (q_cnt == 0)
        return 0;

    int n = (unsigned char)q_pool[q_base] |
            ((unsigned char)q_pool[q_base + 1] << 8);
    int c = n < max ? n : max;
    if (c > 0)
        memcpy(dst, q_pool + q_base + 2, (size_t)c);
    q_base += 2 + n;
    q_fill -= 2 + n;
    q_cnt--;
    if (q_cnt == 0)
        q_base = q_fill = 0;     /* empty: recycle the whole pool */
    return c;
}

void tty_ctrl_deliver(void)
{
    /* Nobody at the console (an idle tick, or only background tasks): the
       keys had no target either, so drop them. */
    int pid = process_console_reader();

    if (ctrl_bs || ctrl_c) {
        if (pid > 0) {
            (void)process_signal(pid, SIGKILL);
            ctrl_c = ctrl_bs = 0;
            ctrl_age = 0;
        } else if (++ctrl_age > 100) {
            /* A full screen program spends most of its time drawing rather
             * than waiting in read(), so for a tick or two there is no
             * console reader to deliver to.  Hold the key for a second
             * (100 ticks) and drop it after that: keeping it forever would
             * fire it at whoever reads the console next. */
            ctrl_c = ctrl_bs = 0;
            ctrl_age = 0;
        }
    } else {
        ctrl_age = 0;
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
