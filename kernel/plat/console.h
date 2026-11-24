#ifndef TINYOS_PLAT_CONSOLE_H
#define TINYOS_PLAT_CONSOLE_H

/* ---- console: the lower half ------------------------------------------
 *
 * A board implements exactly these five calls.  Everything above them lives
 * in the kernel, once: the output lock and the escape/colour filter
 * (console.c), the input ring, line editing, history and echo pacing
 * (tty.c), and the decision of which physical device a byte goes to.
 *
 * The split is not cosmetic; it is what stops the class of bug that cost the
 * C3 port a week: a driver that ran its own state machine, or waited for a
 * peer, from inside the trap handler.  The rules below are all consequences
 * of "this can be called with the output lock held and interrupts off,
 * sometimes from inside the timer interrupt":
 *
 *   - No function here takes a kernel lock, calls the scheduler, sleeps, or
 *     runs a device state machine.  A state machine that needs thread
 *     context belongs in the idle hook (see g_idle_poll), not here.
 *   - Every wait is bounded.  A transmitter that has stopped (gated clock,
 *     no host on the other end) must cost a dropped character, never a
 *     silent board: the bound is what turns "hung" into "garbled".
 *   - putc() may drop.  It is called with the lock held; blocking on a peer
 *     would stall every other core's output with it.
 *   - getchar() must not need more than a register read per device: it runs
 *     from tty_poll(), which is called with interrupts masked and also from
 *     the block driver while a task waits for flash.  Returning -1 because
 *     "nothing to do yet" is correct; driving the device to find out is not.
 *
 * (NuttX splits the same seam into struct uart_ops_s plus drivers/serial.c.
 * We keep the five functions instead of an ops table because there is one
 * console per board and the table would only add an indirect call per
 * character - the same reasoning that keeps our devfs a static table rather
 * than a VFS.) */

void plat_console_init(void);
void plat_console_putc(char c);
/* How many characters plat_console_putc() will take right now without
 * waiting for the transmitter.  Keystroke echo uses this to stay out of the
 * way: echo is produced inside the tick, and a blocking write there delays
 * the next read of the input FIFO - a burst longer than one tick's worth of
 * line time (about 115 bytes at 115200) then overruns a 128-byte receive
 * FIFO and characters are lost.  Return 0 when nothing fits. */
int  plat_console_tx_room(void);
/* Whether the console can take `need` more characters without anything being
 * dropped.  This is for the console write() path, which reports a short count
 * when it says no: boards with a buffered secondary sink (the USB CDC mirror,
 * which drops on a full ring) return 0 while that ring is full, so a bulk
 * writer is paced instead of losing its tail.  The default is "yes" - putc()
 * on a plain UART blocks for a bounded time and never drops, so pacing there
 * would only add syscalls. */
int  plat_console_can_take(int need);
/* Wait until everything written has actually been transmitted.  Called once
 * on the way down: the device may buffer, and the CPU is about to stop.
 * Bounded for the same reason as putc(). */
void plat_console_flush(void);
int  plat_console_getchar(void);      /* -1 when no input is pending */

#endif /* TINYOS_PLAT_CONSOLE_H */
