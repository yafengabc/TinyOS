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
 * What lives here is the part that has to be identical everywhere, because
 * getting it wrong is invisible until a board wedges on a flash write:
 *
 *   - the state and the pure predicates;
 *   - the ordering rules for raising a request and publishing the ack
 *     (rp_park_begin / rp_park_ack), each with the barrier between the data
 *     and the flag;
 *   - the barrier spelling per architecture.
 *
 * The loops themselves stay in each board.  They are not the same code with
 * different spelling: rp2040 has no watchdog feed inside the park loop and no
 * "core 1 must not return" case, rp2350 and rp2350rv disagree about which
 * diagnostics they keep, and the RISC-V wake is a compiler nudge rather than
 * sev.  Extracting those means hook callbacks in the XIP-critical path, which
 * is a bench-session change, not a desk one (issue #17).
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

/* The same rule in two spellings: no later load or store may be observed
 * before the ones that precede this barrier.  dmb is the ARM form; RISC-V
 * needs the explicit rw,rw sets. */
#if defined(__riscv)
#define RP_PARK_BARRIER() __asm__ volatile("fence rw, rw" ::: "memory")
#elif defined(__arm__)
#define RP_PARK_BARRIER() __asm__ volatile("dmb" ::: "memory")
#else
#define RP_PARK_BARRIER() __asm__ volatile("" ::: "memory")
#endif

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

/* Start one handshake and return its generation number.
 *
 * The order is the contract: a new generation number first, then the
 * ownership, then the barrier, and only then the flag.  Raise the flag before
 * the generation moves and the other core acknowledges the previous one; drop
 * the barrier and the core that has not seen the new generation yet can read
 * flag=1 and answer with an ack the owner will accept. */
static inline uint32_t rp_park_begin(struct rp_park_state *s, int me)
{
    uint32_t req = ++s->req;

    s->parked = 0;
    s->owner = me;
    RP_PARK_BARRIER();
    s->flag = 1;
    return req;
}

/* The parked core's answer, called on every pass of its loop.
 *
 * Publishing once on entry is the bug that produced intermittent
 * "dd: write error at block 0 (-1)": a core still inside the loop when the
 * next request is raised never re-enters it, so the new generation waits for
 * an acknowledgement the previous one consumed.  Publishing while the flag is
 * up is safe - the flag is cleared only after XIP is back, so "flag clear"
 * means "you may return to flash". */
static inline void rp_park_ack(struct rp_park_state *s)
{
    if (s->ack != s->req) {
        s->ack = s->req;
        RP_PARK_BARRIER();
    }
}

/* The owner is done: drop the flag and the ownership together.  The caller
 * still has to wake the other core and restore interrupts. */
static inline void rp_park_release(struct rp_park_state *s)
{
    s->flag = 0;
    s->owner = -1;
}

#endif
