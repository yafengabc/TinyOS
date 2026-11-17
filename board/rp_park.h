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
 *   - the barrier and wake spelling per architecture;
 *   - the full enter/exit sequences, RP_PARK_OWNER_TAKE (the core taking the
 *     flash) and RP_PARK_PARKED_LOOP (the core that parks), as macros.
 *
 * The macros are the skeleton; each board supplies only the parts that differ
 * by silicon or by policy, as hook macros defined before this header is
 * included (see the defaults below and the #define blocks at the top of each
 * board's platform.c).  They are macros rather than functions on purpose: the
 * parked loop runs from SRAM while XIP is off, so it must expand into the
 * caller's section.  A function pointer hook would call back into flash and
 * bus-fault the parked core, which is the exact failure this protocol exists
 * to prevent.
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

/* ---- shared enter/exit sequences ---------------------------------------- */

/* The wake and the two barriers, per architecture.  Entering needs a full
 * synchronisation (dsb / fence) so that parked=1, or owner+flag, is visible to
 * the other core before it acts; the per-pass acknowledgement uses the
 * cheaper dmb.  RISC-V has no sev: the parked core is busy-spinning, so the
 * "wake" is a compiler nudge that keeps the shared-memory stores ordered. */
#if defined(__riscv)
#define RP_PARK_ENTER_BARRIER() __asm__ volatile("fence rw, rw" ::: "memory")
#define RP_PARK_WAKE()          __asm__ volatile("slt x0, x0, x1" ::: "memory")
#elif defined(__arm__)
#define RP_PARK_ENTER_BARRIER() __asm__ volatile("dsb" ::: "memory")
#define RP_PARK_WAKE()          __asm__ volatile("sev" ::: "memory")
#else
#define RP_PARK_ENTER_BARRIER() __asm__ volatile("" ::: "memory")
#define RP_PARK_WAKE()          __asm__ volatile("" ::: "memory")
#endif

/* Interrupt masking for the owner.  ARM saves primask and restores it on the
 * timeout path; on success it stays masked because the caller restores once
 * XIP is back.  RISC-V (rp2350rv) masks mstatus.MIE and leaves the restore to
 * the caller on both paths, which already saved it before calling in. */
#if defined(__riscv)
#define RP_PARK_OWNER_SAVE_IRQ(tok)                                          \
    do { (void)(tok); __asm__ volatile("csrci mstatus, 8" ::: "memory"); } while (0)
#define RP_PARK_OWNER_RESTORE_IRQ(tok) do { (void)(tok); } while (0)
#else
#define RP_PARK_OWNER_SAVE_IRQ(tok)                                          \
    do {                                                                     \
        __asm__ volatile("mrs %0, primask" : "=r"(tok));                     \
        __asm__ volatile("cpsid i" ::: "memory");                            \
    } while (0)
#define RP_PARK_OWNER_RESTORE_IRQ(tok)                                       \
    do { if (!((tok) & 1u)) __asm__ volatile("cpsie i" ::: "memory"); } while (0)
#endif

/* Board hooks.  A board overrides the ones it needs by #define-ing them
 * before including this header; the rest fall through to these defaults.
 * They are only ever expanded inside the two sequence macros below, so they
 * may name symbols that are declared later in the same translation unit. */
/* The cadence hooks take the loop counter and decide for themselves; on a
 * board without a watchdog they expand to nothing at all (no read of the
 * volatile counter, so the loop code stays identical to the pre-refactor one).
 * WDT_TICK runs in the parked loop; OWNER_TICK runs in the owner's wait. */
#ifndef RP_PARK_WDT_TICK
#define RP_PARK_WDT_TICK(spins) do { } while (0)
#endif
#ifndef RP_PARK_STOP_FOREVER
#define RP_PARK_STOP_FOREVER() do { } while (0)
#endif
#ifndef RP_PARK_PARKED_DIAG
#define RP_PARK_PARKED_DIAG(entering) do { (void)(entering); } while (0)
#endif
#ifndef RP_PARK_OWNER_PREPARE
#define RP_PARK_OWNER_PREPARE() do { } while (0)
#endif
#ifndef RP_PARK_OWNER_ACKED
#define RP_PARK_OWNER_ACKED() do { } while (0)
#endif
#ifndef RP_PARK_OWNER_TICK
#define RP_PARK_OWNER_TICK(spins) do { } while (0)
#endif
#ifndef RP_PARK_OWNER_TIMEOUT
#define RP_PARK_OWNER_TIMEOUT() do { } while (0)
#endif

/* The owner's whole sequence: mask interrupts, raise the request, wake the
 * other core, wait for this exact generation, and on timeout give up cleanly.
 * Expands to the body of the board's rp_flash_park_other(); it therefore ends
 * every path with a return.
 *
 *   0  success, interrupts still masked (the caller restores after XIP is
 *      back and the other core is unparked);
 *  -1  the other core never parked, interrupts restored (ARM) or left to the
 *      caller (RISC-V); the caller must not take XIP away.
 *
 * `spins_limit` is the board's plat_pre_tick_spins(...) budget. */
#define RP_PARK_OWNER_TAKE(s, spins_limit)                                   \
    uint32_t _rp_tok = 0;                                                    \
    uint32_t _rp_req;                                                        \
    if (cpu_count <= 1)                                                      \
        return 0;                                                            \
    RP_PARK_OWNER_SAVE_IRQ(_rp_tok);                                         \
    RP_PARK_OWNER_PREPARE();                                                 \
    _rp_req = rp_park_begin((s), plat_cpu_id());                             \
    RP_PARK_WAKE();                                                          \
    for (uint32_t _rp_i = 0; _rp_i < (spins_limit); _rp_i++) {               \
        if (rp_park_acked((s), _rp_req)) {                                   \
            RP_PARK_OWNER_ACKED();                                           \
            return 0;                                                        \
        }                                                                    \
        RP_PARK_OWNER_TICK(_rp_i);                                           \
    }                                                                        \
    rp_park_release((s));                                                    \
    RP_PARK_WAKE();                                                          \
    RP_PARK_OWNER_TIMEOUT();                                                 \
    RP_PARK_OWNER_RESTORE_IRQ(_rp_tok);                                      \
    return -1;

/* The parked core's whole loop, expanded into the board's RAM-resident
 * rp_flash_park_loop().  It answers every generation (not once on entry), so
 * a request raised while this core is still inside the loop is acknowledged
 * rather than left waiting for an ack the previous request already consumed.
 * `spins_limit` bounds a writer that never clears the flag so a recoverable
 * error does not turn into a silent wedge. */
#define RP_PARK_PARKED_LOOP(s, spins_limit)                                  \
    RP_PARK_PARKED_DIAG(1);                                                  \
    (s)->parked = 1;                                                         \
    RP_PARK_ENTER_BARRIER();                                                 \
    for (volatile uint32_t _rp_sp = 0; _rp_sp < (spins_limit); _rp_sp++) {   \
        if (!(s)->flag)                                                      \
            break;                                                           \
        RP_PARK_BARRIER();                                                   \
        rp_park_ack((s));                                                    \
        RP_PARK_WDT_TICK(_rp_sp);                                            \
    }                                                                        \
    RP_PARK_PARKED_DIAG(0);                                                  \
    RP_PARK_STOP_FOREVER();                                                  \
    (s)->parked = 0;

/* The owner is done: drop the flag and wake the other core. */
#define RP_PARK_RELEASE_AND_WAKE(s)                                          \
    do { rp_park_release((s)); RP_PARK_WAKE(); } while (0)

#endif
