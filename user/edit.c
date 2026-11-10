#include "lib.h"

/* edit - a small full screen editor.
 *
 * nano's model, not vi's: there are no modes, a printable key inserts at the
 * cursor, and everything else is on the bottom line.  The buffer idea is the
 * same as nano's too - the whole file lives in one array, and the line index
 * is rebuilt after every edit.  At 8 KiB that costs a memmove of a few
 * kilobytes per keystroke, which on a 160 MHz C3 is microseconds and saves
 * the bookkeeping (and the bugs) a gap buffer would need.
 *
 * The limits are set by the task window, not by taste: a program has 64 KiB
 * for image + .bss + heap + a 16 KiB stack, and this one spends ~10 KiB of
 * it on the text and the line index.
 *
 * Saving uses create(), which truncates - the whole buffer is written back,
 * so there is no in-place difference to apply and no half-updated file. */

#define ED_TEXT_MAX 8192        /* bytes of text                    */
#define ED_LINE_MAX 512         /* lines we can index               */
#define ROWS        24          /* rows in the terminal we assume   */
#define COLS        80
#define TEXT_ROWS   (ROWS - 3)  /* title + status + help lines      */

/* Keys that are not a single character.  Negative so they cannot collide
 * with a byte from read(), and K_ERR keeps them from being mistaken for a
 * failed read: a first version tested `k < 0` and every arrow key quit. */
#define K_ERR   (-1)
#define K_UP    (-2)
#define K_DOWN  (-3)
#define K_LEFT  (-4)
#define K_RIGHT (-5)
#define K_HOME  (-6)
#define K_END   (-7)
#define K_DEL   (-8)

static char           text[ED_TEXT_MAX];
static int            text_len;
static unsigned short line_off[ED_LINE_MAX];
static int            nlines;

static int  cy, cx;             /* cursor: line, column in that line */
static int  top;                /* first line on screen              */
static int  hscroll;            /* first column shown, cursor's line */
static int  dirty;
static int  quit_armed;
static char fname[64];
static char msg[56];

static void set_msg(const char *s)
{
    int i = 0;
    while (s[i] && i < (int)sizeof(msg) - 1) {
        msg[i] = s[i];
        i++;
    }
    msg[i] = '\0';
}

/* ---- buffer ---------------------------------------------------------- */

static void reindex(void)
{
    nlines = 1;
    line_off[0] = 0;
    for (int i = 0; i < text_len && nlines < ED_LINE_MAX; i++) {
        if (text[i] == '\n')
            line_off[nlines++] = (unsigned short)(i + 1);
    }
}

static int line_len(int ln)
{
    int start = line_off[ln];
    int end = (ln + 1 < nlines) ? line_off[ln + 1] - 1 : text_len;
    return end - start;
}

static int insert_bytes(int pos, const char *s, int n)
{
    if (text_len + n > ED_TEXT_MAX)
        return -1;
    if (nlines >= ED_LINE_MAX) {
        for (int i = 0; i < n; i++)
            if (s[i] == '\n')
                return -1;
    }
    memmove(text + pos + n, text + pos, (size_t)(text_len - pos));
    memcpy(text + pos, s, (size_t)n);
    text_len += n;
    reindex();
    return 0;
}

static void delete_range(int pos, int n)
{
    if (pos < 0 || n <= 0 || pos + n > text_len)
        return;
    memmove(text + pos, text + pos + n, (size_t)(text_len - pos - n));
    text_len -= n;
    reindex();
}

/* ---- cursor ---------------------------------------------------------- */

/* Keep the cursor's column visible.  Lines longer than the screen are
 * common in config files, and an editor that silently loses the cursor
 * past column 80 is worse than one that scrolls sideways. */
static void fix_hscroll(void)
{
    int w = COLS - 2;
    if (cx < hscroll)
        hscroll = cx;
    if (cx >= hscroll + w)
        hscroll = cx - w + 1;
    if (hscroll < 0)
        hscroll = 0;
}

static void clamp_cx(void)
{
    int len = line_len(cy);
    if (cx > len)
        cx = len;
    if (cx < 0)
        cx = 0;
    fix_hscroll();
}

/* ---- drawing --------------------------------------------------------- */

/* The last frame, one string per screen row, trailing blanks trimmed.  A
 * row is only sent when its content differs from this, so typing one
 * character costs one line and the status line instead of the whole 24-row
 * screen - on a 115200-baud console that is the difference between ~15 ms
 * and ~170 ms per keystroke. */
static char scr[ROWS][COLS + 1];

static void render_row(int r, char *dst)
{
    int n = 0;

    dst[0] = '\0';
    if (r == 0) {
        snprintf(dst, COLS + 1, "  TinyOS edit: %s%s", fname,
                      dirty ? "   [modified]" : "");
        return;
    }
    if (r <= TEXT_ROWS) {
        int ln = top + r - 1;

        if (ln < nlines) {
            int len = line_len(ln);
            int col0 = (ln == cy) ? hscroll : 0;

            if (col0 > len)
                col0 = len;
            n = len - col0;
            if (n > COLS - 1)
                n = COLS - 1;
            if (n > 0)
                memcpy(dst, text + line_off[ln] + col0, (size_t)n);
        }
        dst[n] = '\0';
        return;
    }
    if (r == TEXT_ROWS + 1) {
        snprintf(dst, COLS + 1, " line %d/%d  col %d  %s",
                      cy + 1, nlines, cx + 1, msg);
        return;
    }
    snprintf(dst, COLS + 1,
                  " ^S save  ^X exit  ^K cut line  ^A/^E line ends");
}

static void draw(void)
{
    char row[COLS + 1];
    char seq[24];
    int hidden = 0;

    for (int r = 0; r < ROWS; r++) {
        int n;

        render_row(r, row);
        n = (int)strlen(row);
        while (n > 0 && row[n - 1] == ' ')
            row[--n] = '\0';
        if (!strcmp(row, scr[r]))
            continue;

        if (!hidden) {                  /* no cursor flicker mid-frame */
            puts("\x1b[?25l");
            hidden = 1;
        }
        snprintf(seq, sizeof(seq), "\x1b[%d;1H\x1b[K", r + 1);
        puts(seq);
        if (n > 0)
            write_all(1, row, n);
        memcpy(scr[r], row, (size_t)n + 1);
    }

    snprintf(seq, sizeof(seq), "\x1b[%d;%dH", cy - top + 2,
                  cx - hscroll + 1);
    puts(seq);
    if (hidden)
        puts("\x1b[?25h");
}

static int key_pending(void)
{
    struct pollfd p;

    p.fd = 0;
    p.events = POLLIN;
    p.revents = 0;
    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

/* ---- editing --------------------------------------------------------- */

static void do_insert(int c)
{
    char ch = (char)c;
    if (insert_bytes(line_off[cy] + cx, &ch, 1) < 0) {
        set_msg("buffer full");
        return;
    }
    cx++;
    dirty = 1;
    fix_hscroll();
}

static void do_enter(void)
{
    static const char nl = '\n';
    if (insert_bytes(line_off[cy] + cx, &nl, 1) < 0) {
        set_msg("buffer full or too many lines");
        return;
    }
    cy++;
    cx = 0;
    hscroll = 0;
    dirty = 1;
}

static void do_tab(void)
{
    /* Tabs become spaces on the way in: the display then has one byte per
     * column and the cursor arithmetic stays honest. */
    static const char sp[4] = { ' ', ' ', ' ', ' ' };
    if (insert_bytes(line_off[cy] + cx, sp, 4) < 0) {
        set_msg("buffer full");
        return;
    }
    cx += 4;
    dirty = 1;
    fix_hscroll();
}

static void do_backspace(void)
{
    if (cx > 0) {
        delete_range(line_off[cy] + cx - 1, 1);
        cx--;
    } else if (cy > 0) {
        int prev_len = line_len(cy - 1);
        delete_range(line_off[cy] - 1, 1);      /* the newline above */
        cy--;
        cx = prev_len;
    } else {
        return;
    }
    dirty = 1;
    fix_hscroll();
}

static void do_delete(void)
{
    if (cx < line_len(cy) || cy + 1 < nlines) {
        delete_range(line_off[cy] + cx, 1);
        dirty = 1;
    }
}

static void do_cut_line(void)
{
    int start = line_off[cy];
    int len = line_len(cy);
    if (cy + 1 < nlines) {
        delete_range(start, len + 1);           /* line and its newline */
    } else if (cy > 0) {
        delete_range(start - 1, len + 1);       /* newline above + line */
        cy--;
    } else {
        delete_range(start, len);
    }
    cx = 0;
    dirty = 1;
    fix_hscroll();
}

static void do_save(void)
{
    int fd = create(fname);
    if (fd < 0) {
        set_msg("cannot create file");
        return;
    }
    int off = 0;
    while (off < text_len) {
        int n = write(fd, text + off, text_len - off);
        if (n <= 0)
            break;
        off += n;
    }
    close(fd);
    if (off == text_len) {
        dirty = 0;
        set_msg("saved");
    } else {
        set_msg("write failed");
    }
}

/* ---- input ----------------------------------------------------------- */

static int getkey(void)
{
    unsigned char c, a, b;

    if (read(0, &c, 1) != 1)
        return K_ERR;
    if (c != 27)
        return c;

    /* ESC: a cursor key is ESC [ A..D (and ESC [ 3 ~ for Delete).  A lone
     * ESC waits for the next byte, which is acceptable: nobody uses it in
     * this editor. */
    if (read(0, &a, 1) != 1 || a != '[')
        return 27;
    if (read(0, &b, 1) != 1)
        return 27;

    switch (b) {
    case 'A': return K_UP;
    case 'B': return K_DOWN;
    case 'C': return K_RIGHT;
    case 'D': return K_LEFT;
    case 'H': return K_HOME;
    case 'F': return K_END;
    case '3':
        if (read(0, &b, 1) == 1 && b == '~')
            return K_DEL;
        return K_ERR;
    default:
        return K_ERR;
    }
}

/* ---- file ------------------------------------------------------------ */

static void load_file(void)
{
    int fd = open(fname);
    int truncated = 0;
    int nl = 1;
    text_len = 0;

    if (fd >= 0) {
        char buf[128];
        int n;
        while (!truncated && (n = read(fd, buf, (int)sizeof(buf))) > 0) {
            for (int i = 0; i < n; i++) {
                char c = buf[i];
                if (c == '\r')
                    continue;
                if (c == '\t') {
                    int pad = 4 - (text_len & 3);
                    while (pad-- > 0) {
                        if (text_len >= ED_TEXT_MAX) {
                            truncated = 1;
                            break;
                        }
                        text[text_len++] = ' ';
                    }
                    continue;
                }
                if (c == '\n' && nl >= ED_LINE_MAX) {
                    truncated = 1;
                    break;
                }
                if (text_len >= ED_TEXT_MAX) {
                    truncated = 1;
                    break;
                }
                if (c == '\n')
                    nl++;
                text[text_len++] = c;
            }
        }
        close(fd);
    }

    reindex();
    if (truncated)
        set_msg("truncated: file too large");
}

/* Apply one key.  Returns 0 when the editor should exit. */
static int handle_key(int k)
{
    /* Any key but ^X disarms the quit, so "press ^X twice" really
     * means twice in a row. */
    if (k != 24)
        quit_armed = 0;

    if (k == K_UP) {
        if (cy > 0)
            cy--;
        clamp_cx();
    } else if (k == K_DOWN) {
        if (cy + 1 < nlines)
            cy++;
        clamp_cx();
    } else if (k == K_LEFT) {
        if (cx > 0)
            cx--;
        fix_hscroll();
    } else if (k == K_RIGHT) {
        if (cx < line_len(cy))
            cx++;
        fix_hscroll();
    } else if (k == K_HOME) {
        cx = 0;
        fix_hscroll();
    } else if (k == K_END) {
        cx = line_len(cy);
        fix_hscroll();
    } else if (k == K_DEL) {
        do_delete();
    } else if (k == '\r' || k == '\n') {
        do_enter();
    } else if (k == 8 || k == 127) {
        do_backspace();
    } else if (k == 9) {
        do_tab();
    } else if (k == 19) {               /* ^S */
        do_save();
    } else if (k == 24) {               /* ^X */
        if (dirty && !quit_armed) {
            quit_armed = 1;
            set_msg("unsaved changes - ^X again to quit, ^S to save");
        } else {
            return 0;
        }
    } else if (k == 11) {               /* ^K */
        do_cut_line();
    } else if (k == 1) {                /* ^A */
        cx = 0;
        fix_hscroll();
    } else if (k == 5) {                /* ^E */
        cx = line_len(cy);
        fix_hscroll();
    } else if (k == 12) {               /* ^L: full redraw */
        puts("\x1b[2J");
        memset(scr, 0, sizeof(scr));
    } else if (k >= 32 && k < 127) {
        do_insert(k);
    }

    /* Keep the cursor on screen. */
    if (cy < top)
        top = cy;
    if (cy >= top + TEXT_ROWS)
        top = cy - TEXT_ROWS + 1;
    if (top < 0)
        top = 0;
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts("usage: edit <file>\n");
        return 1;
    }
    if (strlen(argv[1]) >= sizeof(fname)) {
        puts("edit: name too long\n");
        return 2;
    }
    strcpy(fname, argv[1]);

    load_file();

    int old = ttyraw(1);
    puts("\x1b[2J\x1b[H");

    int running = 1;
    while (running) {
        /* Handle everything already queued before the next frame, so a
         * paste costs one draw instead of one per byte.  poll() is what
         * makes that a question we can ask without blocking. */
        while (running && key_pending()) {
            int k = getkey();

            if (k == K_ERR) {
                running = 0;
                break;
            }
            running = handle_key(k);
        }
        if (!running)
            break;

        draw();
        if (key_pending())
            continue;               /* burst: keep coalescing */

        msg[0] = '\0';
        int k = getkey();           /* idle: sleep until a key arrives */
        if (k == K_ERR)
            break;
        running = handle_key(k);
    }

    puts("\x1b[2J\x1b[H");
    ttyraw(old);
    return 0;
}
