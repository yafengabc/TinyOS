#ifndef TINYOS_TIMER_H
#define TINYOS_TIMER_H

#include "types.h"

void timer_init(uint32_t hz);
uint32_t timer_hz(void);
void timer_tick(void);
uint32_t timer_ticks(void);
uint32_t timer_uptime_ms(void);

#endif
