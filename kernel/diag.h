#ifndef TINYOS_DIAG_H
#define TINYOS_DIAG_H

#include "platform.h"

/* Crash and liveness record, compiled out unless `diag = true` is set in the
 * config's [features] section (build turns it into -DTOS_DIAG).
 *
 * Why it exists: the failures this kernel actually had were silent.  A core
 * that takes a bus fault inside a flash window, or spins on a lock whose
 * holder was pre-empted onto it, stops ticking, prints nothing, and is
 * indistinguishable from a dead board - the only evidence a serial capture
 * gives is "the output stopped here".  Debuggers are awkward here (the RP2350
 * SWD pins share solder pads on this board) and an SWO trace costs a probe
 * plus a working clock, so the kernel keeps its own record instead:
 *
 *   live   - per-core flags and a tick heartbeat, so one core can tell
 *            whether the other is wedged inside a masked section, spinning,
 *            or parked for a flash write *right now*;
 *   last   - where it was when it stopped (site id, caller's LR, PC at the
 *            last tick), so the report names a line of source after the fact.
 *
 * Everything lands in .noinit, which the linker places above .bss and which
 * _start does not clear, so a watchdog reset leaves the record intact and the
 * next boot prints it as one [PREV] line.  That is the whole design: no host
 * tooling, no probe, one serial capture. */

/* Placed above .bss in a NOLOAD section: a watchdog reset does not clear
 * SRAM and _start only clears [__bss_start, __bss_end), so the state that
 * wedged the previous boot is still readable on the next one.  Never give one
 * of these an initialiser - a NOLOAD section has nothing to load it from, and
 * the variable would start life as garbage. */
#define TOS_NOINIT __attribute__((section(".noinit")))

/* Sites inside plat_irq_save/disable and the SIO spin gate, recorded in
 * g_mask_site so a wedged core names the kind of section it never left.
 * 5 is the flash-park handshake, which the board layer records itself. */
#define TOS_MASKSITE_IRQ_SAVE      1u
#define TOS_MASKSITE_IRQ_DISABLE   2u
#define TOS_MASKSITE_SPIN_TRY_GATE 3u
#define TOS_MASKSITE_SPIN_GATE     4u
#define TOS_MASKSITE_FLASH_PARK    5u

#if TOS_DIAG

/* "Now", not "last": set for as long as the core is inside the region. */
TOS_NOINIT extern volatile uint32_t g_tick_ctr[2];   /* ticks delivered per core */
TOS_NOINIT extern volatile uint32_t g_mask_in[2];    /* inside a masked section */
TOS_NOINIT extern volatile uint32_t g_spin_in[2];    /* spinning for a kernel lock */
TOS_NOINIT extern volatile uint32_t g_park_in[2];    /* parked for a flash write */

/* Where it happened, for the report the next boot prints. */
TOS_NOINIT extern volatile uint32_t g_tick_pc[2];    /* PC when a tick landed */
TOS_NOINIT extern volatile uint32_t g_tick_lr[2];
TOS_NOINIT extern volatile uint32_t g_mask_seq[2];   /* masked sections entered */
TOS_NOINIT extern volatile uint32_t g_mask_lr[2];    /* caller of the last one */
TOS_NOINIT extern volatile uint32_t g_mask_site[2];  /* TOS_MASKSITE_* */
TOS_NOINIT extern volatile uint32_t g_spin_wait[2];  /* lock-wait retries */
TOS_NOINIT extern volatile uint32_t g_spin_lock_addr[2];
TOS_NOINIT extern volatile uint32_t g_spin_caller[2];
TOS_NOINIT extern volatile uint32_t g_spin_count[2];
TOS_NOINIT extern volatile uint32_t g_idle_site;     /* step in sched_idle */
TOS_NOINIT extern volatile uint32_t g_usb_site[2];   /* step in the CDC driver */
TOS_NOINIT extern volatile uint32_t g_cpu_idle[2];

/* Board hook for the same purpose: called from a spin-wait loop so a board
 * can report what both cores are doing while one of them is stuck.  Weak
 * no-op default in cpu.c. */
void plat_diag_poll(void);

#else

#define plat_diag_poll() do { } while (0)

#endif

#endif
