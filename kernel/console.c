#include "console.h"
#include "hwdev.h"
#include "platform.h"
#include "process.h"
#include "cpu.h"
#include "string.h"

/* Architecture independent console: formatting + routing to the platform's
 * character output.  The VGA text buffer, UART or any other device lives
 * behind plat_console_putc(). */

/* A line of output must not be cut in half by another task or by the kernel
 * printing its own message: a task preempted between two puts() leaves a
 * line open, the next task's text continues it, and the rest of the first
 * task's text lands after that (a killed task's "[pid 2] killed" splitting
 * "spin[2] 1/6 at ...").  out_owner remembers who is mid-line and
 * console_adopt_line() closes the line when somebody else takes the console
 * over, before their first character.  The kernel prints as owner 0, which
 * no task is, and keystroke echo is its own owner so that it never
 * interrupts the task that is printing.
 *
 * Keystroke echo is queued, not written through.  tty_poll() runs inside an
 * interrupt and cannot wait for the transmitter, so console_echo() only
 * puts bytes in a ring and the tick drains what the device will take.
 * Any synchronous output path has to drain the rest of that ring FIRST:
 * those bytes belong to keystrokes that arrived before the output existed,
 * and letting a task print first puts a pasted command's tail after the
 * prompt instead of before it. */
#define CONSOLE_ECHO_OWNER (-1)

static int            out_owner;       /* pid whose line is still open      */
static int            out_mid;         /* last character was not a newline  */

/* One lock for the character stream.  The out_* bookkeeping above assumes a
 * single writer: on an SMP machine two tasks printing at the same moment
 * would shuffle their characters together and both would see the other as
 * "somebody who just interrupted me", producing a line break every few
 * characters.
 *
 * The granularity is one output call - a whole kprintf() or one write() to
 * the console - so a formatted line stays contiguous.  Separate writes can
 * still interleave, which is exactly what an SMP system does with printf
 * per character; the pieces are whole lines or whole buffers, never halves.
 *
 * Non-recursive.  out_char() is the inner loop and never takes it; the few
 * public entry points below do, and the internal helpers assume it is
 * already held.  It can be held while plat_console_putc() waits for a
 * transmitter, which is why nothing that blocks (reading input, sleeping)
 * ever runs under it. */
static spin_t         out_lock = SPIN_INIT;

/* The one place a character leaves the kernel.
 *
 * The panel, when there is one, is a second screen rather than a second
 * driver: it gets the same byte stream the UART gets, including the echo,
 * so the LCD shows what the serial terminal shows.  Doing it here - below
 * out_char(), adopt_line() and the echo queue - is what keeps those three
 * from each needing to know a screen exists.
 *
 * screen_mirror_putc() returns immediately when mirroring is off or when
 * the board has no panel, so this costs one call per character on boards
 * that do not. */
static void con_putc(char c)
{
    plat_console_putc(c);
    screen_mirror_putc(c);
}

static void out_char(char c)
{
    int who = process_current_pid();

    if (who != out_owner && out_mid) {
        con_putc('\r');
        con_putc('\n');
    }
    out_owner = who;
    out_mid = (c != '\n');
    if (c == '\n')
        con_putc('\r');
    con_putc(c);
}

/* Lock already held. */
static void adopt_line_locked(void)
{
    if (!out_mid || process_current_pid() == out_owner)
        return;
    out_owner = process_current_pid();
    out_mid = 0;
    con_putc('\r');
    con_putc('\n');
}

void console_adopt_line(void)
{
    if (!spin_trylock(&out_lock))
        return;
    adopt_line_locked();
    spin_unlock(&out_lock);
}

/* The console is dying with the machine: push the last characters out and
 * stop everything that could still print.  The device is drained first
 * because halting the CPU drops whatever is still sitting in a boxed output
 * queue (an emulated 16550 has exactly that problem). */
void console_shutdown(void)
{
    plat_console_flush();
    plat_irq_disable();
}

/* Show a keystroke, on its own line if a program has one open.
 *
 * Echo is queued, not written through.  tty_poll() runs inside an interrupt
 * and echoes every byte it cooked; writing those synchronously means waiting
 * for the transmitter, and at 115200 baud 180 characters take 15.6 ms -
 * longer than the 1 ms tick.  Nothing drains the receive FIFO during that
 * wait, so an input burst longer than the FIFO depth (128 bytes on the C3)
 * is truncated in the middle: a 180-character pasted command reached the
 * shell as two fragments and failed with "sh: name too long", and the file
 * it was meant to write came out 63 characters long.  Hand typing never
 * shows it because a human produces a byte every ~100 ms.
 *
 * console_echo() queues, console_echo_tick() moves only what the device will
 * take without blocking, and every synchronous output path drains whatever
 * is left before its own first byte.  A full queue drops the newest echo:
 * the characters are already in the line buffer, and losing sight of what
 * you typed is recoverable in a way that losing the input is not.
 */
#define ECHO_Q 256

static char echo_q[ECHO_Q];
static int  echo_head, echo_tail;      /* ring: head == tail means empty */

void console_echo(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        int next = (echo_tail + 1) % ECHO_Q;
        if (next == echo_head)
            return;                    /* full: drop, the input itself is safe */
        echo_q[echo_tail] = s[i];
        echo_tail = next;
    }
}

/* Move queued echo bytes out, at most `max` of them (-1 = all of them).
 * The lock is already held.  This is deliberately NOT out_char(): echo may
 * start in the middle of a task's open line (the shell prompt), and closing
 * that line here would put every typed character on a fresh line.  Echo
 * simply takes ownership of the line, exactly as the tick path always did.
 *
 * The tick passes the transmitter's free space so it never blocks; the
 * synchronous output paths pass -1, because they are about to wait for the
 * transmitter anyway and must not let a keystroke land after their text. */
static void echo_drain_locked(int max)
{
    if (echo_head == echo_tail)
        return;
    while (echo_head != echo_tail && (max < 0 || max-- > 0)) {
        char c = echo_q[echo_head];
        echo_head = (echo_head + 1) % ECHO_Q;
        out_owner = CONSOLE_ECHO_OWNER;
        out_mid = (c != '\n');
        if (c == '\n')
            con_putc('\r');
        con_putc(c);
    }
}

void console_echo_tick(void)
{
    int room = plat_console_tx_room();

    if (room <= 0 || echo_head == echo_tail)
        return;
    if (!spin_trylock(&out_lock))
        return;                    /* the queue keeps its bytes; next tick */
    echo_drain_locked(room);
    spin_unlock(&out_lock);
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

static void ob_uint(struct outbuf *o, uint32_t v, uint32_t base, int width,
                    char pad, int upper)
{
    char tmp[16];
    int k = 0;
    if (v == 0)
        tmp[k++] = '0';
    while (v) {
        uint32_t d = v % base;
        tmp[k++] = (char)(d < 10 ? '0' + d
                                 : (upper ? 'A' : 'a') + d - 10);
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
    ob_uint(o, u, 10, width, pad, 0);
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

        /* The 'l' length modifier is a no-op on every target here (int and
         * long are both 32-bit).  TinyUSB's debug logs use %lu/%lX, and
         * silently not consuming the argument would misalign every following
         * one. */
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l')
                fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i':
            ob_int(&o, __builtin_va_arg(ap, int), width, pad);
            break;
        case 'u':
            ob_uint(&o, __builtin_va_arg(ap, unsigned int), 10, width, pad, 0);
            break;
        case 'x':
            ob_uint(&o, __builtin_va_arg(ap, unsigned int), 16, width, pad, 0);
            break;
        case 'X':
            ob_uint(&o, __builtin_va_arg(ap, unsigned int), 16, width, pad, 1);
            break;
        case 'p':
            ob_uint(&o, __builtin_va_arg(ap, unsigned int), 16, 8, '0', 0);
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
    /* A kernel line stands on its own: close any task line it interrupts.
     *
     * Try-lock, not spin: printing is best effort, and a core parked in
     * plat_spin_acquire() with interrupts off cannot be preempted - if the
     * holder is waiting for something that needs *this* core, spinning here
     * hangs the machine.  Dropping a line under contention costs a log
     * entry; spinning costs the board. */
    if (!spin_trylock(&out_lock))
        return;
    echo_drain_locked(-1);
    adopt_line_locked();
    for (int i = 0; i < n; i++)
        out_char(buf[i]);
    spin_unlock(&out_lock);
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

/* Console output lock.  A task's own output must not be thrown away when the
 * lock is busy: on SMP the holder is usually the other core draining queued
 * echo in its 1 ms tick, and dropping a whole write there is visible as "the
 * command printed nothing at all" - measured on rp2350rvsmp, where any command
 * line of 16 bytes or more (`cat  /readme.txt`, `ls /proc/xxxxxxx`) lost both
 * the program's output and the following prompt while the command itself ran
 * to completion.  Sixteen bytes of echo fit in the drain that happens before
 * the program starts; seventeen did not, so the tick still held the lock when
 * the write arrived.
 *
 * Waiting is only safe when the holder is the OTHER core.  When it is this
 * core the caller is an interrupt landing inside our own critical section, and
 * waiting would deadlock - there dropping is the only answer.  Kernel logging
 * (kprintf) keeps its own try-lock rule: a line of diagnostics is worth
 * losing to keep a wedged board printable. */
static volatile int out_lock_cpu = -1;

static int out_lock_take(void)
{
    if (spin_trylock(&out_lock)) {
        out_lock_cpu = (int)plat_cpu_id();
        return 1;
    }
    if (out_lock_cpu == (int)plat_cpu_id())
        return 0;                       /* re-entry from this core: drop */
    spin_lock(&out_lock);               /* the other core: wait it out   */
    out_lock_cpu = (int)plat_cpu_id();
    return 1;
}

static void out_lock_give(void)
{
    out_lock_cpu = -1;
    spin_unlock(&out_lock);
}

void console_putc(char c)
{
    if (!out_lock_take())
        return;
    echo_drain_locked(-1);
    out_char(c);
    out_lock_give();
}

void console_write(const char *s, size_t n)
{
    if (!out_lock_take())
        return;
    echo_drain_locked(-1);
    for (size_t i = 0; i < n; i++)
        out_char(s[i]);
    out_lock_give();
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
    console_adopt_line();
    for (int i = 0; i < n; i++)
        out_char(buf[i]);
    kprintf("\nSystem halted.\n");
    plat_panic_halt();
}
