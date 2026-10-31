#ifndef TINYOS_TIMER_H
#define TINYOS_TIMER_H

#include "types.h"

void timer_init(void);
void timer_tick(void);
uint32_t timer_ticks(void);
uint32_t timer_uptime_ms(void);
void timer_sleep_ms(uint32_t ms);

#endif
