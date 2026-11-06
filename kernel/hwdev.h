#ifndef TINYOS_HWDEV_H
#define TINYOS_HWDEV_H

#include "types.h"
#include "tos.h"      /* TOS_DEV_*, LCD_OP_* - shared with user space */

/* On-board devices.
 *
 * kernel/periph.c abstracts the two things every MCU has (a pin, an ADC
 * channel).  A board also has things a pin cannot describe: a panel with a
 * resolution, a button with a de-bounce policy, a card slot.  Those are
 * described here, as a table the board hands over plus a handful of class
 * level hooks.
 *
 * Two rules keep this from turning into a driver zoo:
 *
 *   - the table is data, not code.  A board says "this is a LED on GPIO3,
 *     active low" and gets /dev/led0 for free.  Nothing in the kernel
 *     knows the part number.
 *   - the hooks are per class, never per device.  There is one
 *     plat_display_cell(), and a board with two panels decides which one
 *     it is by index.
 *
 * Everything is `weak` in kernel/hwdev.c, so a board that has no panel
 * simply does not implement the hook, the class reports 0 devices, and no
 * /dev node appears - the same "ask first, then use" contract periph.c
 * uses for GPIO. */

typedef struct board_dev {
    const char *name;    /* "led0", "key0", "lcd0" - the /dev name          */
    const char *model;   /* "ST7789V2 135x240" - what user space prints    */
    uint8_t     cls;     /* TOS_DEV_*                                       */
    uint8_t     flags;   /* TOS_DEV_* flags                                 */
    int16_t     pin;     /* primary GPIO, -1 when there is none             */
    int16_t     aux;     /* class specific: bus, channel, or a second pin   */
    const char *detail;  /* "spi1 sck=10 mosi=11 dc=8 cs=9 bl=25"           */
} board_dev_t;

/* The board's table, and how many entries it has.  Weak: a board that says
 * nothing gets zero devices and an empty /proc/dev, not a link error. */
const board_dev_t *plat_board_devs(int *count);

/* "ATK-DNRP2350AM" and friends.  Printed once at boot and by /proc/dev. */
const char *plat_board_name(void);

/* ---- LEDs ------------------------------------------------------------ */

int plat_led_count(void);
/* Logical level: 1 lights the LED.  Polarity is the board's business, so
 * every caller above this line says "on", never "low". */
int plat_led_set(int idx, int on);
int plat_led_get(int idx);            /* 0/1, or -1 when not readable */

/* ---- buttons --------------------------------------------------------- */

int plat_key_count(void);
/* 1 while held, 0 while released, -1 when the board has no such key.
 * Debouncing belongs to the board: a hook that reports a 2 ms contact
 * bounce makes `cat /dev/key0` useless. */
int plat_key_read(int idx);

/* ---- display --------------------------------------------------------- */

int plat_display_count(void);
/* Returns 0 and fills the geometry in PIXELS (`w` x `h`, the addressable
 * panel) plus the character cell the panel draws text with.  The text grid
 * is w/cell_w by h/cell_h; the driver deliberately does not report that,
 * because only the screen layer knows what font is in use.  Any pointer
 * may be NULL. */
int plat_display_info(int idx, int *w, int *h, int *cell_w, int *cell_h);
/* Paint one text cell.  `c` outside the printable range draws a space. */
int plat_display_cell(int idx, int col, int row, char c,
                      uint16_t fg, uint16_t bg);
/* Paint a whole text row in one bus transaction.  `chars` holds `ncols`
 * cells of the given row; returns 0 when the board has a fast path, -1
 * when it does not and the caller must fall back to plat_display_cell().
 * Both hooks may return before the pixels have left the chip (DMA); the
 * display layer serialises them, callers never see the bus. */
int plat_display_row(int idx, int row, const char *chars, int ncols,
                     uint16_t fg, uint16_t bg);
/* Blit one horizontal run of RGB565 pixels at absolute pixel (x,y).
 * `px` is w*2 bytes, high byte first - the panel's native order.  Returns
 * 0 or -1; weak default -1 means the board has no pixel path. */
int plat_display_scan(int idx, int x, int y, int w, const uint8_t *px);
/* Block until everything previously queued has actually left the chip.
 * Anything that wants to share the display's bus (the TF card slot on
 * this board shares GPIO10/11) must call this before switching the
 * chip-selects. */
void plat_display_sync(int idx);
/* Fill the whole panel.  Only used at init and by LCD_OP_CLEAR. */
int plat_display_clear(int idx, uint16_t color);
int plat_display_backlight(int idx, int on);
int plat_display_backlight_get(int idx);

/* ---- boot wiring ----------------------------------------------------- */

/* Build the /dev nodes for whatever the board reported.  Called once from
 * kmain(), next to periph_init_devs(). */
void hwdev_init(void);

/* The text screen on display 0: a console-shaped view of the panel with a
 * cursor, wrapping and scrolling.  kernel/console.c feeds it a copy of
 * everything it prints when mirroring is on; programs reach the same
 * screen through /dev/lcdN. */
int  screen_init(void);
void screen_putc(char c);
void screen_puts(const char *s);
void screen_clear(uint16_t color);
int  screen_cols(void);
int  screen_rows(void);
void screen_mirror(int on);
int  screen_mirror_on(void);
/* Console mirror: one character, dropped when mirroring is off.  This is
 * what console.c calls from out_char(), so it must not block on anything
 * but the panel itself and must never print. */
void screen_mirror_putc(char c);

/* Cursor, colours and inverse video, the parts LCD_OP_* exposes. */
void screen_cursor(int col, int row);
int  screen_cursor_col(void);
int  screen_cursor_row(void);
void screen_colors(uint16_t fg, uint16_t bg);
uint16_t screen_fg(void);
uint16_t screen_bg(void);
void screen_invert(int on);
int  screen_inverted(void);


#endif
