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

/* Register /dev/gpioN and /dev/adcN for whatever this board reports.  Called
 * once at boot, before the first task exists. */
void periph_init_devs(void);

#endif
