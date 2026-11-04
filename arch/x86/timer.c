#include "timer.h"
#include "io.h"

/* The PIT's input clock.  The divisor is derived from the requested rate
 * rather than stored as a constant, so a build that asks kmain for 1000 Hz
 * gets 1000 Hz - and timer_uptime_ms() divides by the same number the
 * hardware is programmed with. */
#define PIT_BASE_HZ 1193182u

static volatile uint32_t ticks;
static uint32_t pit_hz = 100u;

void timer_init(uint32_t hz)
{
    /* The 16-bit divisor caps this at 18 Hz; below that the PIT would need a
     * cascade, and nothing in this tree asks for a rate that slow. */
    if (hz < 19u)
        hz = 19u;
    pit_hz = hz;

    uint32_t divisor = PIT_BASE_HZ / hz;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

uint32_t timer_hz(void)
{
    return pit_hz;
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
    /* Whole milliseconds per tick: exact for the rates this tree uses (100,
     * 1000) and one multiply rather than a 64-bit divide the kernel would
     * have to link a helper for. */
    return ticks * (1000u / pit_hz);
}
