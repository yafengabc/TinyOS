#ifndef TINYOS_RP_PARK_H
#define TINYOS_RP_PARK_H

/* Flash park protocol shared by rp2040, rp2350 and rp2350rv.
 *
 * The core that needs the flash (the "owner") raises `flag`, wakes the other
 * core with sev/event, and waits until `ack == req`.  The other core parks in
 * the idle hook or plat_flash_park_poll(), publishes the ack from RAM, and
 * spins until the owner clears the flag.  Matching request numbers, not a
 * boolean, is what makes a second handshake distinguishable from a stale ack
 * left by the previous one.
 *
 * This header owns the state and the pure predicates.  The entry/exit
 * sequences stay in each board for now: a wrong barrier or IRQ restore here
 * kills XIP for the whole machine, so the sequence extraction waits for a
 * bench session where all three boards can run a flash soak
 * (OPEN-ITEMS 2f).
 */

#include "types.h"

struct rp_park_state {
    volatile uint32_t req;    /* incremented by the core taking the flash */
    volatile uint32_t ack;    /* last request the parked core has seen    */
    volatile int      flag;   /* non-zero while a park is requested       */
    volatile int      parked; /* the other core is spinning in the loop   */
    volatile int      owner;  /* core id that owns the flash, -1 = none   */
};

#define RP_PARK_STATE_INIT { 0u, 0u, 0, 0, -1 }

/* Called from the idle hook and the poll gate: this core must park, unless it
 * is the one that asked for the flash. */
static inline int rp_park_should_park(const struct rp_park_state *s, int me)
{
    return s->flag && s->owner != me;
}

/* The owner's wait: has the other core acknowledged this exact request? */
static inline int rp_park_acked(const struct rp_park_state *s, uint32_t req)
{
    return s->ack == req;
}

/* The owner is done: drop the flag and the ownership together.  The caller
 * still has to sev and restore interrupts. */
static inline void rp_park_release(struct rp_park_state *s)
{
    s->flag = 0;
    s->owner = -1;
}

#endif
