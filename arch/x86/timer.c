#include "timer.h"
#include "serial.h"
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
    serial_poll(); /* keep input alive while a user program is running */
}

uint32_t timer_ticks(void)
{
    return ticks;
}

uint32_t timer_uptime_ms(void)
{
    return ticks * (1000u / PIT_HZ);
}

void timer_sleep_ms(uint32_t ms)
{
    uint32_t want = ms / (1000u / PIT_HZ);
    uint32_t target = ticks + (want ? want : 1);
    while ((int32_t)(ticks - target) < 0) {
        __asm__ volatile("sti; hlt");
    }
}
