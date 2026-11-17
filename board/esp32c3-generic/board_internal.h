#ifndef TINYOS_BOARD_ESP32C3_INTERNAL_H
#define TINYOS_BOARD_ESP32C3_INTERNAL_H

#include "types.h"

/* APB clock as measured at boot: 80 MHz when PLL_SYS came up (the normal
 * case at both 80 and 160 MHz CPU), 20 MHz when the port had to stay on the
 * ROM's XTAL/2.  The SPI and I2C drivers scale their dividers from it.
 *
 * It is a separate number from plat_cpu_hz(): the C3 can run the CPU at
 * 160 MHz with APB at 80, so "cpu/2" is only right in one of the cases. */
uint32_t c3_apb_hz(void);

#endif
