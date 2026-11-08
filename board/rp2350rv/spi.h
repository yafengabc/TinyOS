#ifndef TINYOS_RP2350_SPI_H
#define TINYOS_RP2350_SPI_H

#include "types.h"

/* Polled SPI master.  Bus is 0 or 1; `div` is the PL022 clock divider
 * (even, >= 2), so the bus runs at clk_peri / div.  With clk_peri at
 * 150 MHz (PLL_SYS), div 4 is 37.5 MHz and div 12 is 12.5 MHz.  The
 * panel adds a DMA layer on top of this for its bulk transfers (see
 * lcd_st7789.c); this file stays polled for short command bytes. */
void rp_spi_init(int bus, uint32_t div);
int  rp_spi_write(int bus, const void *buf, size_t n);
int  rp_spi_write_byte(int bus, uint8_t b);

#endif
