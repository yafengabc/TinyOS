#ifndef TINYOS_PERIPH_H
#define TINYOS_PERIPH_H

#include "types.h"
#include "tos.h"      /* GPIO_OP_* / ADC_OP_*, shared with user space */

/* Peripheral front end.
 *
 * The board supplies plat_gpio_* / plat_adc_* (kernel/platform.h); this
 * module turns those hooks into the two multiplexed syscalls, so adding an
 * operation later does not consume another syscall number. */

int periph_gpio(int op, int pin, int arg);
int periph_adc(int op, int chan);
int periph_pwm(int op, int gpio, int freq_hz, int duty_permille);
int periph_uart_count(void);
int periph_uart_write(int port, const void *buf, int len);
int periph_uart_read(int port, void *buf, int len);
int periph_spi_count(void);
int periph_spi_xfer(int bus, int cs_gpio, int freq_hz, int mode,
                    const void *tx, void *rx, int len);
int periph_i2c_count(void);
int periph_i2c_xfer(int bus, int addr, int freq_hz,
                    const void *tx, int txlen, void *rx, int rxlen);
int periph_dma_memcpy(void *dst, const void *src, int len);

/* Register /dev/gpioN and /dev/adcN for whatever this board reports.  Called
 * once at boot, before the first task exists. */
void periph_init_devs(void);

#endif
