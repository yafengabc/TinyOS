#ifndef TINYOS_RP2040_UART_DBG_H
#define TINYOS_RP2040_UART_DBG_H

#include "types.h"

/* Polled UART0 on GPIO0 (TX) / GPIO1 (RX), 115200 8N1.
 *
 * This exists for bring-up only: USB is the product console, but when the
 * USB stack is the thing being debugged there has to be a second wire.  The
 * functions never block for longer than one character and are safe to call
 * from anywhere, including before the scheduler exists.
 *
 * `clk_hz` is clk_peri.  After clocks_init() it is clk_sys at 125 MHz; the
 * early boot ROM path may still be at 12 MHz, which is why the caller passes
 * the measured value instead of assuming one. */
void dbg_uart_init(uint32_t clk_hz);
/* Repoint only the PL011 baud divisor when clk_peri changes underneath it.
 * The caller writes this BEFORE moving the clock mux and emits no byte in
 * between; it deliberately does not touch the pads / FIFO / enable bits. */
void dbg_uart_set_peri_hz(uint32_t clk_hz);
void dbg_uart_putc(char c);
void dbg_uart_puts(const char *s);
void dbg_uart_hex(uint32_t v);

#endif
