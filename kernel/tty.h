#ifndef TINYOS_TTY_H
#define TINYOS_TTY_H

#include "types.h"

/* Console line discipline.
 *
 * Input is polled from the timer tick (neither the 16550 nor the PS/2 port
 * is wired to an interrupt here), cooked into a single line buffer, and
 * collected by whoever is reading.  Cooking happens whether or not anybody
 * is reading, so typing echoes immediately and no process has to sit in the
 * kernel spinning on the UART while others wait for the CPU. */

void tty_poll(void);                       /* drain the device, cook it   */
int  tty_line_ready(void);
int  tty_take_line(char *dst, int max);    /* copy and clear the line     */

#endif
