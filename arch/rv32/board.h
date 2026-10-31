#ifndef TINYOS_RV32_BOARD_H
#define TINYOS_RV32_BOARD_H

#include "types.h"

/* Contract between the generic RV32 layer (arch/rv32/platform.c) and the
 * board support package (board/<board>/platform.c).
 *
 * The generic layer owns everything that is pure CSR / trap / PMP work and
 * is identical across RV32 boards: trap dispatch, user-mode entry, PMP
 * programming, interrupt enable/disable, the panic halt.  The board owns
 * the peripherals, which differ completely (QEMU virt: 16550 + CLINT +
 * RAM disk; ESP32-C3: Espressif UART0 + SYSTIMER + flash-mapped disk).
 *
 * Hook points called from the generic layer:
 *
 *   plat_board_early()   start of plat_early_init(): bring up the console
 *   plat_board_init()    end of plat_init(): timers, watchdogs, devices
 *   plat_timer_tick()    machine timer interrupt: re-arm the comparator
 *                        and advance the tick counter (read via
 *                        plat_ticks(), which the board also supplies) */

void plat_board_early(void);
void plat_board_init(void);
void plat_timer_tick(void);

/* MMIO helpers shared by both layers.  RV32 has no I/O space, so every
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
