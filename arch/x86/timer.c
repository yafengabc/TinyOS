#include "timer.h"
#include "io.h"

#define PIT_HZ 100

static volatile uint32_t ticks;

void timer_init(void)
{
    uint32_t divisor = 1193182u / PIT_HZ;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

void timer_tick(void)
{
    ticks++;
}

uint32_t timer_ticks(void)
{
    return ticks;
}

uint32_t timer_uptime_ms(void)
{
    return ticks * (1000u / PIT_HZ);
}
