#ifndef _HARDWARE_SYNC_H
#define _HARDWARE_SYNC_H

/* TinyOS shim: memory barriers and the interrupt save/restore pair.  The
 * RP2040 port includes this header for __dmb/__dsb around DPRAM writes. */

#include "types.h"

static inline void __dsb(void) { __asm__ volatile("dsb 0xf" ::: "memory"); }
static inline void __dmb(void) { __asm__ volatile("dmb 0xf" ::: "memory"); }
static inline void __isb(void) { __asm__ volatile("isb 0xf" ::: "memory"); }
static inline void __sev(void) { __asm__ volatile("sev"); }
static inline void __wfe(void) { __asm__ volatile("wfe"); }

static inline uint32_t save_and_disable_interrupts(void)
{
    uint32_t primask;
    __asm__ volatile("mrs %0, primask\n cpsid i" : "=r"(primask) :: "memory");
    return primask;
}

static inline void restore_interrupts(uint32_t status)
{
    __asm__ volatile("msr primask, %0" :: "r"(status) : "memory");
}

#endif
