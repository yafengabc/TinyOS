/* Host harness for the text-screen layer (kernel/screen.c).
 *
 * Why this exists: with the panel mirror on, a `cat` of a file containing a
 * TAB froze every ATK image (console silent, idle loop gone, watchdog after
 * ~33 s).  putc_locked() expanded the tab by comparing cx against a stop
 * column; when the stop was the last column, the wrap inside the loop reset
 * cx to 0, the comparison kept holding, and the tab repainted for ever
 * (commit 5260d28, kernel/screen.c).
 *
 * The stub panel counts every painted cell and exits the moment one case goes
 * over budget, so that regression fails fast instead of hanging the test.
 *
 * The Go side (internal/build/hosttests_test.go) compiles this against the
 * real screen.c and requires exit status 0 plus "screen ok" in the output.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "string.h"     /* shim: memmove/memcpy/memset declarations */
#include "cpu.h"        /* shim: spin_t, SPIN_INIT, spin_lock/unlock   */

/* Public API of the module under test. */
int  screen_init(void);
void screen_putc(char c);
void screen_cursor(int col, int row);
int  screen_cursor_col(void);
int  screen_cursor_row(void);
int  screen_cols(void);
int  screen_rows(void);
void screen_clear(uint16_t color);

/* Geometry the stubbed panel reports.  COLS must be a multiple of 4 for the
 * ordinary tab cases and the "last column" ones to be the same column; 40 is
 * the ATK panel's real width. */
#define W_PX    320
#define H_PX    256
#define CELL_W  8
#define CELL_H  16
#define COLS    (W_PX / CELL_W)
#define ROWS    (H_PX / CELL_H)

#define PAINT_LIMIT 20000       /* a screenful is COLS*ROWS == 640 */

static int  paints;             /* plat_display_cell calls          */
static int  rows_painted;       /* plat_display_row calls           */
static int  clears;
static int  failures;
static char grid[ROWS][COLS];

static void fail(const char *what)
{
    printf("FAIL %s (paints=%d rows=%d)\n", what, paints, rows_painted);
    exit(2);                    /* a runaway is a failure, not a wait */
}

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

/* ---- the panel the screen layer talks to -------------------------------- */

int plat_display_count(void)
{
    return 1;
}

int plat_display_info(int idx, int *w, int *h, int *cell_w, int *cell_h)
{
    (void)idx;
    if (w)
        *w = W_PX;
    if (h)
        *h = H_PX;
    if (cell_w)
        *cell_w = CELL_W;
    if (cell_h)
        *cell_h = CELL_H;
    return 0;
}

int plat_display_cell(int idx, int col, int row, char c,
                      uint16_t fg, uint16_t bg)
{
    (void)idx; (void)fg; (void)bg;
    if (col < 0 || col >= COLS || row < 0 || row >= ROWS) {
        printf("FAIL cell out of range %d,%d\n", col, row);
        exit(2);
    }
    if (++paints > PAINT_LIMIT)
        fail("paint runaway");
    grid[row][col] = c;
    return 0;
}

int plat_display_row(int idx, int row, const char *chars, int ncols,
                     uint16_t fg, uint16_t bg)
{
    (void)idx; (void)fg; (void)bg;
    if (row < 0 || row >= ROWS || ncols != COLS) {
        printf("FAIL row out of range %d/%d\n", row, ncols);
        exit(2);
    }
    if (++rows_painted > PAINT_LIMIT)
        fail("row runaway");
    memcpy(grid[row], chars, (size_t)ncols);
    return 0;                   /* fast path exists: scroll goes through it */
}

int plat_display_clear(int idx, uint16_t color)
{
    (void)idx; (void)color;
    clears++;
    memset(grid, ' ', sizeof grid);
    return 0;
}

int plat_display_backlight(int idx, int on)
{
    (void)idx; (void)on;
    return 0;
}

int plat_display_backlight_get(int idx)
{
    (void)idx;
    return 1;
}

/* Single-threaded host: the lock is a no-op. */
void spin_lock(spin_t *s) { (void)s; }
void spin_unlock(spin_t *s) { (void)s; }

/* ---- cases -------------------------------------------------------------- */

int main(void)
{
    if (screen_init() != 0) {
        printf("FAIL screen_init\n");
        return 2;
    }
    check(screen_cols() == COLS, "grid width follows the panel");
    check(screen_rows() == ROWS, "grid height follows the panel");
    check(clears == 1, "init clears the panel once");

    /* A tab at column 0 fills to the first stop. */
    screen_cursor(0, 0);
    paints = 0;
    screen_putc('\t');
    check(screen_cursor_col() == 4 && screen_cursor_row() == 0, "tab at 0 lands on 4");
    check(paints == 4, "tab at 0 paints 4 cells");

    /* Mid-line: 37 -> 40 wraps to the next row. */
    screen_cursor(37, 0);
    paints = 0;
    screen_putc('\t');
    check(paints == 3, "tab at 37 paints 3 cells");
    check(screen_cursor_col() == 0 && screen_cursor_row() == 1, "tab at 37 wraps");

    /* The regression: the last column.  One cell fits, then the wrap must
     * stop the expansion.  Before 5260d28 this painted until the watchdog. */
    screen_cursor(COLS - 1, 2);
    paints = 0;
    screen_putc('\t');
    check(paints == 1, "tab at the last column paints one cell");
    check(screen_cursor_col() == 0 && screen_cursor_row() == 3,
          "tab at the last column wraps to the next row");
    if (paints > 4)
        fail("tab at the last column expanded more than one stop");

    /* Last row: the wrap scrolls; it must stay bounded and keep the cursor
     * on the last row. */
    screen_cursor(COLS - 1, ROWS - 1);
    paints = 0;
    rows_painted = 0;
    screen_putc('\t');
    check(screen_cursor_row() == ROWS - 1, "tab on the last row keeps the row");
    check(rows_painted == ROWS, "scroll repaints every row once");
    if (paints > 4)
        fail("tab on the last row expanded more than one stop");

    /* Ordinary text still advances, and a newline still wraps. */
    screen_cursor(0, 4);
    screen_putc('x');
    check(screen_cursor_col() == 1, "a character advances the cursor");
    screen_putc('\n');
    check(screen_cursor_col() == 0 && screen_cursor_row() == 5, "newline wraps");

    /* Backspace at (0,0) must not underflow. */
    screen_cursor(0, 0);
    screen_putc('\b');
    check(screen_cursor_col() == 0 && screen_cursor_row() == 0,
          "backspace at 0,0 stays at 0,0");

    if (failures) {
        printf("FAIL %d case(s)\n", failures);
        return 1;
    }
    printf("screen ok\n");
    return 0;
}
