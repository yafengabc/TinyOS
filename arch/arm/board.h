#ifndef TINYOS_ARM_BOARD_H
#define TINYOS_ARM_BOARD_H

#include "types.h"

/* Contract between the generic ARM layer (arch/arm/platform.c) and the
 * board support package (board/<board>/platform.c).
 *
 * The generic layer owns everything that is pure exception / Thread-mode
 * work and is identical across Cortex-M boards: trap dispatch, user-mode
 * entry, interrupt enable/disable, the panic halt.  The board owns the
 * peripherals (UART, SysTick clock, block device, shutdown), which differ
 * completely (QEMU mps2: CMSDK UART + SysTick + RAM disk; an MCU would
 * use its own UART and SysTick clock).
 *
 * Hook points called from the generic layer:
 *
 *   plat_board_early()   start of plat_early_init(): bring up the console
 *   plat_board_init()    end of plat_init(): enable fault handlers, etc.
 *   plat_timer_tick()    SysTick interrupt: re-arm if needed and advance
 *                        the tick counter (read via plat_ticks(), which the
 *                        board also supplies) */

void plat_board_early(void);
void plat_board_init(void);
void plat_timer_tick(void);

/* Called from plat_irq_enable() so a board can unmask a timer interrupt
 * that its plat_timer_init() deliberately left masked. */
void plat_timer_irq_unmask(void);

/* Which core is asking.  The generic layer reads no core-id register: on
 * the RP2040 it is SIO_CPUID, on a single-core M3 there is nothing to read. */
uint32_t plat_board_cpu_id(void);

/* MMIO helpers shared by both layers.  Cortex-M has no I/O space, so every
 * peripheral access is a plain load/store. */
static inline void mmio_write8(uint32_t addr, uint8_t v)
{
    *(volatile uint8_t *)addr = v;
}

static inline uint8_t mmio_read8(uint32_t addr)
{
    return *(volatile uint8_t *)addr;
}

static inline uint32_t mmio_read32(uint32_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void mmio_write32(uint32_t addr, uint32_t v)
{
    *(volatile uint32_t *)addr = v;
}

#endif
