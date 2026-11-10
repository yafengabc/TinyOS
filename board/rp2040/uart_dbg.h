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
 * `clk_hz` is clk_peri: 12 MHz while the boot ROM's settings are still in
 * force, 48 MHz after the USB clock init moves clk_peri onto clk_sys. */
void dbg_uart_init(uint32_t clk_hz);
void dbg_uart_putc(char c);
void dbg_uart_puts(const char *s);
void dbg_uart_hex(uint32_t v);

#endif
