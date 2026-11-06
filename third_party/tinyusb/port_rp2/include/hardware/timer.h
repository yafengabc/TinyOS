#ifndef _HARDWARE_TIMER_H
#define _HARDWARE_TIMER_H

/* TinyOS shim: the free-running 1 MHz system timer.  TIMER0 sits at
 * 0x400b0000 on the RP2350 (the RP2040 had it at 0x40054000) and TIMERAWL
 * latches the low 32 bits of the microsecond counter. */

#include "types.h"

#define TIMER0_BASE     0x400b0000u
#define TIMERAWL_OFFSET 0x00000028u

static inline uint32_t time_us_32(void)
{
    return *(volatile uint32_t *)(TIMER0_BASE + TIMERAWL_OFFSET);
}

static inline uint64_t time_us_64(void)
{
    return (uint64_t)time_us_32();
}

#endif
