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

/* Board-specific task PMP programming.  Return nonzero when the board wrote
 * the PMP itself; the generic layer then leaves it alone.  It exists because
 * Hazard3 on the RP2350 supports NAPOT (and NA4) but not TOR: writing A=TOR
 * maps the region to OFF, so the generic TOR layout silently denies every
 * user fetch.  A board with that core describes the user area with
 * naturally-aligned power-of-two NAPOT regions instead. */
int plat_board_task_protect(uint32_t win_base, uint32_t win_end, uint32_t wx,
                            uint32_t brk, uint32_t stack_low,
                            uint32_t stack_top);

/* Board hook polled from plat_panic_halt().  A board whose console works
 * without interrupts (RP2350's UART0, for instance) can keep watching for a
 * reflash key after the kernel has died, so a crashed image does not need a
 * physical BOOTSEL press.  The generic layer provides an empty weak default;
 * boards that have nothing to poll simply do not define it. */
void plat_board_panic_poll(void);

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
