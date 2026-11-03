#ifndef TINYOS_TTY_H
#define TINYOS_TTY_H

#include "types.h"

/* Console line discipline.
 *
 * Input is polled from the timer tick (neither the 16550 nor the PS/2 port
 * is wired to an interrupt here), cooked into a single line buffer, and
 * collected by whoever is reading.  Cooking happens whether or not anybody
 * is reading, so typing echoes immediately and no process has to sit in the
 * kernel spinning on the UART while others wait for the CPU.
 *
 * One line of type-ahead: anything typed while a line is already waiting is
 * dropped, not queued.  A burst (a paste, or a script feeding several lines
 * at once) therefore delivers exactly one line and loses the rest. */

/* Longest line the console hands out in one read(), newline included.
 * kernel/tty.c keeps its queue in slots of this size. */
#define TTY_LINE_MAX 256

void tty_poll(void);                       /* drain the device, cook it   */
int  tty_line_ready(void);
int  tty_take_line(char *dst, int max);    /* copy one line, clear it     */

/* ---- raw mode --------------------------------------------------------
 * A full-screen program (edit) needs the bytes the cooked path throws away:
 * ESC, the arrow-key sequences built from it, and anything else below 32.
 * While raw mode is on, tty_poll() only queues bytes, read() hands them
 * over immediately, and no echo is produced - the program owns the screen.
 * The flag is console-wide, not per process: only one task can be reading
 * the console at a time, and process.c clears it when that task dies so a
 * killed editor cannot leave the shell without a line discipline. */
void tty_set_raw(int on);
int  tty_is_raw(void);
int  tty_raw_ready(void);
int  tty_take_raw(char *dst, int max);

/* ^C / ^Z / ^\ kill, stop and continue the foreground task (the one blocked
 * reading the console).  Called once per tick after tty_poll(); a no-op when
 * no control character arrived.  The tty layer only knows "the reader", the
 * kernel turns that into a process. */
void tty_ctrl_deliver(void);

#endif
