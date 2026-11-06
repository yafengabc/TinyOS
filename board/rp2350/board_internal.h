#ifndef TINYOS_RP2350_BOARD_INTERNAL_H
#define TINYOS_RP2350_BOARD_INTERNAL_H

/* Pieces shared between this board's files and nothing else.
 *
 * The kernel contract lives in kernel/platform.h and arch/arm/board.h; this
 * header is for facts that only make sense on an ATK-DNRP2350AM. */

/* Has user space claimed the LED yet?
 *
 * plat_timer_tick() blinks the blue LED once a second as a "kernel is
 * alive" signal that needs no serial port.  That is useful exactly until
 * someone writes to /dev/led0 - then the heartbeat would fight them, and
 * an LED that ignores `echo 1 > /dev/led0` is a broken abstraction.  So the
 * first write takes ownership and the blink stops for good. */
int plat_led_owned(void);

#endif
