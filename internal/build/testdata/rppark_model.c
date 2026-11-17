/* Host model of the flash-park handshake rules in board/rp_park.h.
 *
 * The real sequence runs with XIP off, so a mistake there is found by a board
 * wedging on a flash write - which is exactly how the two known bugs
 * (publish-the-ack-once, and ack the wrong generation) were found.  The state
 * machine itself is plain C, so the rules can be exercised on a desk instead:
 * this only calls the header's inline helpers with simulated cores.
 *
 * Built by internal/build/rpparkmodel_test.go, next to a stub types.h.
 */
#include <stdio.h>

#include "rp_park.h"

static int failures;

static void check(int cond, const char *what)
{
    if (!cond)
        failures++;
    printf("%s %s\n", cond ? "ok  " : "FAIL", what);
}

int main(void)
{
    struct rp_park_state s = RP_PARK_STATE_INIT;

    /* 1. One handshake: raise, acknowledge, release. */
    uint32_t g1 = rp_park_begin(&s, 0);
    check(g1 == 1u, "first generation is 1");
    check(!rp_park_acked(&s, g1), "not acked before the other core answers");
    check(rp_park_should_park(&s, 1), "the other core must park");
    check(!rp_park_should_park(&s, 0), "the owner must not park itself");
    rp_park_ack(&s);
    check(rp_park_acked(&s, g1), "acked after one publish");
    rp_park_release(&s);
    check(s.flag == 0 && s.owner == -1, "release drops flag and ownership");
    check(!rp_park_should_park(&s, 1), "nobody parks after release");

    /* 2. A stale acknowledgement must not satisfy the next generation. */
    uint32_t g2 = rp_park_begin(&s, 0);
    check(g2 == g1 + 1u, "generation advances");
    check(!rp_park_acked(&s, g2), "the previous ack does not count for this one");
    rp_park_ack(&s);
    check(rp_park_acked(&s, g2), "this one is acked after a publish");
    rp_park_release(&s);

    /* 3. Back-to-back requests: the second one is raised while the parked core
     *    is still inside its loop, so it never re-enters.  Its loop keeps
     *    publishing, which is why the ack has to be re-published for every
     *    generation rather than once on entry. */
    uint32_t g3 = rp_park_begin(&s, 0);
    rp_park_ack(&s);                       /* core 1 is now in its loop */
    rp_park_release(&s);
    uint32_t g4 = rp_park_begin(&s, 1);    /* the other core takes the flash */
    check(g4 == g3 + 1u, "second request gets a new generation");
    check(!rp_park_acked(&s, g4), "the old ack is not the new one");
    rp_park_ack(&s);                       /* the parked core's next pass */
    check(rp_park_acked(&s, g4), "a core still in its loop acks the new request");
    rp_park_release(&s);

    /* 4. An ack from the previous generation must not be mistaken for this
     *    one even when the parked core has left its loop entirely. */
    uint32_t g5 = rp_park_begin(&s, 0);
    s.ack = g5 - 1u;                       /* stale value a writer may see */
    check(!rp_park_acked(&s, g5), "stale ack value does not ack this request");
    rp_park_ack(&s);
    check(rp_park_acked(&s, g5), "a real publish does");
    rp_park_release(&s);

    printf("%s\n", failures ? "MODEL FAIL" : "MODEL OK");
    return failures ? 1 : 0;
}
