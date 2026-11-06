#include "hwdev.h"
#include "platform.h"
#include "console.h"
#include "string.h"
#include "types.h"
#include "cpu.h"      /* spin_t: the mirror runs inside console output */

/* A text screen on top of a pixel panel.
 *
 * The panel driver only knows how to paint one character cell; everything
 * a console needs - a cursor, wrapping, scrolling, a colour pair, the
 * mirror switch - is here, so a second panel or a different controller
 * costs nothing above the pixel level.
 *
 * The shadow buffer is why there is a buffer at all: scrolling a 135-pixel
 * panel means repainting it, and repainting from the panel's own RAM is not
 * possible (ST7789 has no read path wired on this board).  640 bytes buys
 * correct scrolling instead of a teletype that clears and restarts.
 *
 * Concurrency: one lock covers the buffer and the panel.  The mirror runs
 * inside console output (out_lock held in console.c), so nothing here may
 * call back into the console - it never prints, it never panics. */

#define SCREEN_MAX_COLS 48
#define SCREEN_MAX_ROWS 20

static spin_t lock = SPIN_INIT;

static int  cols, rows;          /* text grid */
static int  cell_w, cell_h;      /* pixels per cell */
static int  cx, cy;
static uint16_t fg = 0xFFFFu;    /* white */
static uint16_t bg = 0x0000u;    /* black */
static int  inverted;
static int  mirror_on;
static int  ready;
static char cells[SCREEN_MAX_ROWS][SCREEN_MAX_COLS];

static void paint_cell(int col, int row)
{
    char c = cells[row][col];
    uint16_t f = inverted ? bg : fg;
    uint16_t b = inverted ? fg : bg;

    plat_display_cell(0, col, row, c, f, b);
}

static void paint_row(int row)
{
    uint16_t f = inverted ? bg : fg;
    uint16_t b = inverted ? fg : bg;

    /* Fast path: one window + one DMA for the whole row, pixels moved in
     * the background.  Boards without the row hook fall back to one cell
     * transaction per column. */
    if (plat_display_row(0, row, cells[row], cols, f, b) != 0)
        for (int c = 0; c < cols; c++)
            paint_cell(c, row);
}

static void paint_all(void)
{
    for (int r = 0; r < rows; r++)
        paint_row(r);
}

static void clear_buffer(char fill)
{
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++)
            cells[r][c] = fill;
}

/* One line up: shift the buffer, blank the last row, repaint everything.
 * The panel has no hardware scroll usable in landscape (MADCTL 0x70 maps
 * the VSCSAD axis sideways), so the repaint is real - but each row goes
 * out as one DMA'd window (~0.8 ms of wire time each at 37.5 MHz), the
 * CPU only queues them and scrolling a full screen costs ~13 ms of bus
 * time, none of it polled. */
static void scroll_up(void)
{
    memmove(cells[0], cells[1], (size_t)(rows - 1) * SCREEN_MAX_COLS);
    for (int c = 0; c < cols; c++)
        cells[rows - 1][c] = ' ';
    paint_all();
}

static void newline(void)
{
    cx = 0;
    if (++cy >= rows) {
        cy = rows - 1;
        scroll_up();
    }
}

static void putc_locked(char c)
{
    if (c == '\n') {
        newline();
        return;
    }
    if (c == '\r') {
        cx = 0;
        return;
    }
    if (c == '\b') {
        if (cx > 0) {
            cx--;
        } else if (cy > 0) {
            cy--;
            cx = cols - 1;
        }
        cells[cy][cx] = ' ';
        paint_cell(cx, cy);
        return;
    }
    if (c == '\t') {
        int stop = (cx + 4) & ~3;
        while (cx < stop && cx < cols)
            putc_locked(' ');
        return;
    }
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e)
        c = '?';         /* the font is ASCII; show a marker, not garbage */

    cells[cy][cx] = c;
    paint_cell(cx, cy);
    if (++cx >= cols)
        newline();
}

int screen_init(void)
{
    int w, h, cw, ch;

    spin_lock(&lock);
    if (ready) {
        spin_unlock(&lock);
        return 0;
    }
    if (plat_display_count() <= 0 ||
        plat_display_info(0, &w, &h, &cw, &ch) < 0) {
        spin_unlock(&lock);
        return -1;
    }
    if (cw <= 0 || ch <= 0) {
        spin_unlock(&lock);
        return -1;
    }

    cols = w / cw;
    rows = h / ch;
    if (cols > SCREEN_MAX_COLS)
        cols = SCREEN_MAX_COLS;
    if (rows > SCREEN_MAX_ROWS)
        rows = SCREEN_MAX_ROWS;
    if (cols <= 0 || rows <= 0) {
        spin_unlock(&lock);
        return -1;
    }

    cell_w = cw;
    cell_h = ch;
    cx = cy = 0;
    clear_buffer(' ');
    plat_display_clear(0, bg);
    plat_display_backlight(0, 1);
    /* A board that went to the trouble of fitting a panel wants the
     * console on it; `screen mirror off` turns it back into a plain
     * framebuffer for programs that would rather draw themselves. */
    mirror_on = 1;
    ready = 1;
    spin_unlock(&lock);
    return 0;
}

void screen_putc(char c)
{
    if (!ready)
        return;
    spin_lock(&lock);
    putc_locked(c);
    spin_unlock(&lock);
}

void screen_puts(const char *s)
{
    if (!ready || !s)
        return;
    spin_lock(&lock);
    while (*s)
        putc_locked(*s++);
    spin_unlock(&lock);
}

void screen_clear(uint16_t color)
{
    if (!ready)
        return;
    spin_lock(&lock);
    bg = color;
    clear_buffer(' ');
    cx = cy = 0;
    plat_display_clear(0, color);
    spin_unlock(&lock);
}

void screen_mirror(int on)
{
    if (!ready)
        return;
    spin_lock(&lock);
    mirror_on = on != 0;
    spin_unlock(&lock);
}

int screen_mirror_on(void)
{
    return mirror_on;
}

void screen_mirror_putc(char c)
{
    if (!ready || !mirror_on)
        return;
    screen_putc(c);
}

void screen_cursor(int col, int row)
{
    if (!ready)
        return;
    spin_lock(&lock);
    if (col >= 0 && col < cols)
        cx = col;
    if (row >= 0 && row < rows)
        cy = row;
    spin_unlock(&lock);
}

int screen_cursor_col(void) { return cx; }
int screen_cursor_row(void) { return cy; }
int screen_cols(void) { return cols; }
int screen_rows(void) { return rows; }

void screen_colors(uint16_t f, uint16_t b)
{
    if (!ready)
        return;
    spin_lock(&lock);
    fg = f;
    bg = b;
    paint_all();
    spin_unlock(&lock);
}

uint16_t screen_fg(void) { return fg; }
uint16_t screen_bg(void) { return bg; }

void screen_invert(int on)
{
    if (!ready)
        return;
    spin_lock(&lock);
    inverted = on != 0;
    paint_all();
    spin_unlock(&lock);
}

int screen_inverted(void) { return inverted; }
