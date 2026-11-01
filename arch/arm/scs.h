#ifndef TINYOS_ARCH_SCS_H
#define TINYOS_ARCH_SCS_H

#include "types.h"

/* ARMv7-M System Control Space (SCS): the core's own registers, present on
 * every Cortex-M.  This is architecture, not board, so the map lives here
 * and both the generic layer and the boards may include it.
 *
 * The SCS holds the SysTick timer, the NVIC and the exception priority
 * registers.  Board maps (arm_memmap.h) add their peripherals on top. */

#define ARM_SCS_BASE     0xE000E000u

/* Exception priorities.  Each system handler owns one byte: SHPR2 puts
 * SVCall in [31:24], SHPR3 puts SysTick in [31:24] and PendSV in [23:16].
 * Cortex-M3 implements the top three bits, so values step by 0x20 from
 * 0x00 (highest) to 0xE0 (lowest); writing a value that is not a multiple
 * of 0x20 simply loses the unimplemented low bits. */
#define ARM_SHPR2        (ARM_SCS_BASE + 0xD1Cu)
#define ARM_SHPR3        (ARM_SCS_BASE + 0xD20u)

/* SysTick outranks SVCall: a task blocked inside its syscall idles with
 * SVCall active, and only a higher-priority exception can preempt it.
 * PendSV stays at the bottom for the future unprivileged switch. */
#define ARM_PRIO_SYSTICK 0x00u
#define ARM_PRIO_SVC     0x40u
#define ARM_PRIO_PENDSV  0xE0u

#endif
