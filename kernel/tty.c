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
static int  pending_cur;                /* insertion point, 0..pending_len */

/* ESC sequence state for the arrow keys.  The cooked path used to drop
 * everything below 32, so ESC [ A arrived as nothing at all; now the
 * sequence is recognised without letting its bytes into the line. */
#define ESC_NONE 0
#define ESC_SEEN 1                      /* got ESC                        */
#define ESC_BRKT 2                      /* got ESC [                      */
#define ESC_NUM  3                      /* got ESC [ 3 (Delete)           */
static int  esc_state;

/* ---- command history --------------------------------------------------
 * A pool of finished lines, each stored as one length byte plus its text:
 * 512 bytes is ~43 typical commands, against 8 KiB for 32 fixed 256-byte
 * slots.  The pool is linear and recycles from the front when full - the
 * same shape q_pool uses for type-ahead.  It lives here rather than in the
 * shell because the arrow keys are cooked here; the shell never sees them.
 *
 * One pool size everywhere: 512 bytes.  The C3 briefly ran a 192-byte pool
 * because its kernel window was full to within 368 bytes; the .bss trims
 * (process table, exec argv buffer, device table) bought back 5.4 KiB, so
 * the small-pool special case is gone.  A board that cannot spare it can
 * still define TOS_HIST_BYTES=0 and get the stub below.
 *
 * hist_pos is 0 while a fresh line is being edited, 1 for the newest
 * entry, 2 for the one before it, and so on. */
#ifndef TOS_HIST_BYTES
#define TOS_HIST_BYTES 512
#endif

#if TOS_HIST_BYTES > 0
#define HIST_BYTES TOS_HIST_BYTES
/* The line stashed when history recall starts.  Small pools get a small
 * stash: a board this tight is also not typing 200-character commands. */
#if HIST_BYTES >= 512
#define HIST_KEEP_MAX TTY_LINE_MAX
#else
#define HIST_KEEP_MAX 64
#endif
static char hist_pool[HIST_BYTES];
static int  hist_used;                  /* bytes in use, oldest first      */
static int  hist_cnt;                   /* entries                         */
static int  hist_pos;
static char hist_keep[HIST_KEEP_MAX];   /* line being edited before recall */
static int  hist_keep_len;
#endif

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
 * that reader is out of the CPU.  With a 1 ms slice (process.c) a reader that
 * is merely preempted costs one tick, i.e. 12 bytes at 115200; the depth that
 * matters is the reader that is *running* and not reading - rx committing a
 * chunk to flash, an editor repainting.
 *
 * Measured, not derived (user/rawsink.c + scripts/esp-c3/rawstorm.py): a
 * reader that goes away for a burst keeps exactly RAW_Q - 1 bytes, and the
 * 128-byte hardware FIFO adds nothing to that, because the receive interrupt
 * empties the FIFO into this ring as it goes.  So this number *is* the whole
 * buffer, and 511 vs 1023 bytes is 44 vs 89 ms of wire at 115200.
 *
 * 512 is enough because the file protocol is stop-and-wait and rx writes the
 * chunk *before* it answers: the sender is waiting for the ACK exactly while
 * the flash is busy, so nothing arrives during the one unbounded stall.  A
 * 64 KiB transfer verifies at the same 7.7 KiB/s as it did at 1024. */
#define RAW_Q 512
static char raw_q[RAW_Q];
static int  raw_head, raw_tail;        /* ring: head == tail means empty */
static int  raw_on;

void tty_set_raw(int on)
{
    cr_seen = 0;
    esc_state = ESC_NONE;
#if TOS_HIST_BYTES > 0
    hist_pos = 0;
#endif
    if (on) {
        /* Drop a half-typed command: it belongs to the shell, and the
         * editor's first keystroke must not complete it. */
        pending_len = 0;
        pending_cur = 0;
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

/* ---- line editing helpers --------------------------------------------
 * Everything here redraws with backspace and spaces only: ESC [ K would be
 * shorter, but the LCD text screen implements '\b' by moving its cursor and
 * has no escape parser, and the same bytes have to look right on both the
 * panel and a serial terminal.  Moving right means re-printing the tail,
 * which is exactly what a real terminal would do with a cursor-right it
 * does not have. */
static void echo_bs(int n)
{
    while (n-- > 0)
        console_echo("\b", 1);
}

static void echo_tail(int from)
{
    if (from < pending_len)
        console_echo(pending + from, (size_t)(pending_len - from));
}

/* Move the insertion point, redrawing only what has to change. */
static void move_cur(int to)
{
    if (to < 0)
        to = 0;
    if (to > pending_len)
        to = pending_len;
    if (to < pending_cur) {
        echo_bs(pending_cur - to);
    } else if (to > pending_cur) {
        echo_tail(pending_cur);
        echo_bs(pending_len - to);
    }
    pending_cur = to;
}

/* Wipe the typed text from the screen and the buffer. */
static void kill_line(void)
{
    echo_bs(pending_cur);
    for (int i = 0; i < pending_len; i++)
        console_echo(" ", 1);
    echo_bs(pending_len);
    pending_len = 0;
    pending_cur = 0;
}

static void insert_char(char c)
{
    if (pending_len >= TTY_LINE_MAX - 2)
        return;
    memmove(pending + pending_cur + 1, pending + pending_cur,
            (size_t)(pending_len - pending_cur));
    pending[pending_cur] = c;
    pending_len++;
    if (pending_cur == pending_len - 1) {
        console_echo(&c, 1);            /* appending: nothing to redraw */
        pending_cur++;
    } else {
        echo_tail(pending_cur);         /* print the shifted tail ...   */
        echo_bs(pending_len - pending_cur - 1);
        pending_cur++;                  /* ... and step back over it    */
    }
}

static void delete_char(void)
{
    if (pending_cur >= pending_len)
        return;
    memmove(pending + pending_cur, pending + pending_cur + 1,
            (size_t)(pending_len - pending_cur - 1));
    pending_len--;
    echo_tail(pending_cur);
    console_echo(" ", 1);               /* erase the vacated last cell  */
    echo_bs(pending_len - pending_cur + 1);
}

/* Delete the word before the cursor (^W): skip trailing blanks, then the
 * word itself.  Rewrites from the start of the typed text because the
 * deleted run is in the middle, then walks the cursor back. */
static void delete_word(void)
{
    int from = pending_cur, removed;

    while (from > 0 && pending[from - 1] == ' ')
        from--;
    while (from > 0 && pending[from - 1] != ' ')
        from--;
    if (from == pending_cur)
        return;
    removed = pending_cur - from;
    memmove(pending + from, pending + pending_cur,
            (size_t)(pending_len - pending_cur));
    pending_len -= removed;
    pending_cur = from;
    echo_bs(from + removed);            /* back to the first typed cell */
    echo_tail(0);                       /* reprint the shortened line   */
    for (int i = 0; i < removed; i++)
        console_echo(" ", 1);           /* erase what moved left        */
    echo_bs(pending_len - from + removed);
}

/* ---- history ---------------------------------------------------------- */
#if TOS_HIST_BYTES > 0


/* Offset of the payload of the k-th newest entry (1 = newest), -1 when
 * there is no such entry.  The pool is oldest-first, so walking it is a
 * loop over at most a few dozen length bytes. */
static int hist_entry(int k, int *lenp)
{
    int want, off = 0;

    if (k < 1 || k > hist_cnt)
        return -1;
    want = hist_cnt - k;
    for (int i = 0; i < want; i++)
        off += 1 + (unsigned char)hist_pool[off];
    *lenp = (unsigned char)hist_pool[off];
    return off + 1;
}

/* Keep a finished line, dropping the oldest entries when the pool is full.
 * Consecutive duplicates are folded, which is what every interactive shell
 * does and what keeps `ls`, Enter, Enter from eating the pool. */
static void hist_push(const char *s, int n)
{
    int last, lastlen, off;

    if (n <= 0)
        return;
    last = hist_entry(1, &lastlen);
    if (last >= 0 && lastlen == n && memcmp(hist_pool + last, s, (size_t)n) == 0)
        return;

    while (hist_used + 1 + n > HIST_BYTES && hist_cnt > 0) {
        int len = (unsigned char)hist_pool[0];
        memmove(hist_pool, hist_pool + 1 + len, (size_t)(hist_used - 1 - len));
        hist_used -= 1 + len;
        hist_cnt--;
    }
    if (hist_used + 1 + n > HIST_BYTES)
        return;                         /* a single line larger than the pool */

    off = hist_used;
    hist_pool[off] = (char)n;
    memcpy(hist_pool + off + 1, s, (size_t)n);
    hist_used += 1 + n;
    hist_cnt++;
}

/* Replace the line being edited with history entry k (0 = the line that
 * was being typed before recall started). */
static void hist_show(int k)
{
    int len = 0, off = -1;

    if (k > 0) {
        off = hist_entry(k, &len);
        if (off < 0)
            return;             /* entry vanished (pool recycled); keep line */
    }
    kill_line();
    if (k == 0) {
        len = hist_keep_len;
        if (len > TTY_LINE_MAX - 2)
            len = TTY_LINE_MAX - 2;
        memcpy(pending, hist_keep, (size_t)len);
    } else {
        if (len > TTY_LINE_MAX - 2)
            len = TTY_LINE_MAX - 2;
        memcpy(pending, hist_pool + off, (size_t)len);
    }
    pending_len = len;
    pending_cur = len;
    console_echo(pending, (size_t)pending_len);
    hist_pos = k;
}

/* Up: older.  The first step stashes the current line so Down can bring it
 * back, exactly like bash. */
static void hist_up(void)
{
    if (hist_cnt == 0 || hist_pos >= hist_cnt)
        return;
    if (hist_pos == 0) {
        hist_keep_len = pending_len;
        memcpy(hist_keep, pending, (size_t)pending_len);
    }
    hist_show(hist_pos + 1);
}

static void hist_down(void)
{
    if (hist_pos == 0)
        return;
    hist_show(hist_pos - 1);
}

#else  /* TOS_HIST_BYTES == 0: no pool, no recall, a few stubs */
static void hist_push(const char *s, int n) { (void)s; (void)n; }
static void hist_up(void) { }
static void hist_down(void) { }
int tty_hist_count(void) { return 0; }
int tty_hist_get(int i, char *dst, int max) { (void)i; (void)dst; (void)max; return -1; }
#endif

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
        pending_cur = 0;
        cr_seen = 0;
        esc_state = ESC_NONE;
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

    /* ---- escape sequences from the arrow keys ------------------------ */
    if (esc_state != ESC_NONE || c == 27) {
        if (c == 27) {
            esc_state = ESC_SEEN;
            return;
        }
        if (esc_state == ESC_SEEN && c == '[') {
            esc_state = ESC_BRKT;
            return;
        }
        if (esc_state == ESC_BRKT) {
            esc_state = ESC_NONE;
            switch (c) {
            case 'A': hist_up();   return;   /* up    */
            case 'B': hist_down(); return;   /* down  */
            case 'C': move_cur(pending_cur + 1); return;
            case 'D': move_cur(pending_cur - 1); return;
            case 'H': move_cur(0);            return;
            case 'F': move_cur(pending_len);  return;
            case '3': esc_state = ESC_NUM;    return;
            default:                          return;
            }
        }
        if (esc_state == ESC_NUM) {
            esc_state = ESC_NONE;
            if (c == '~')
                delete_char();
            return;
        }
        esc_state = ESC_NONE;
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
        hist_push(pending, n);
        pending_len = 0;
        pending_cur = 0;
#if TOS_HIST_BYTES > 0
        hist_pos = 0;
#endif
        esc_state = ESC_NONE;
        return;
    }

    if (c == 8 || c == 127) {
        if (pending_cur > 0) {
            /* Step the screen cursor back one cell, then delete at it. */
            echo_bs(1);
            pending_cur--;
            delete_char();
        }
        return;
    }

    /* Line editing, in the same spirit as a terminal in canonical mode. */
    switch (c) {
    case 1:  move_cur(0);            return;   /* ^A home        */
    case 5:  move_cur(pending_len);  return;   /* ^E end         */
    case 11: {                                 /* ^K kill to end */
        int n = pending_len - pending_cur;
        for (int i = 0; i < n; i++)
            console_echo(" ", 1);
        echo_bs(n);
        pending_len = pending_cur;
        return;
    }
    case 21:                                   /* ^U kill line   */
        kill_line();
        return;
    case 23:                                   /* ^W kill word   */
        delete_word();
        return;
    case 9:                                    /* TAB: a space   */
        insert_char(' ');
        return;
    default:
        break;
    }

    if (c >= 32 && c < 127)
        insert_char(c);
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

#if TOS_HIST_BYTES > 0
int tty_hist_count(void)
{
    return hist_cnt;
}

int tty_hist_get(int newest_index, char *dst, int max)
{
    int len, off;

    off = hist_entry(newest_index, &len);
    if (off < 0)
        return -1;
    if (len > max - 1)
        len = max - 1;
    memcpy(dst, hist_pool + off, (size_t)len);
    dst[len] = '\0';
    return len;
}
#endif

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
    /* The console reader is the foreground task in the usual sense; a job a
     * blocked parent waits for is the same task one level up (`run spin`
     * never reads, so without it ^C could not stop one at all); and the task
     * outranking the shell is what is left when a promoted job neither
     * reads nor waits.  Only when none of the three exists - an idle tick,
     * or only the shell at its prompt - is there nothing to deliver to. */
    int pid = process_ctrl_target();

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
