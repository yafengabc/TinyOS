#include "platform.h"
#include "console.h"
#include "board_config.h"

/* The generated header (from board.conf) and layout.c's constants are two
 * declarations of the same numbers.  layout.c is what the window allocator,
 * the linker and the protection unit actually use; board.conf is what the
 * build validates against the chip and what B-1c will derive Targets from.
 * If they drift, every window on the board is sized from one number and
 * checked against the other, so refuse to boot instead of guessing.
 *
 * The strings are here so the check cannot be optimised into "compare a
 * constant with itself" when a future B-1c makes the C side read generated
 * values directly. */
void board_config_check(void)
{
    if (PLAT_APP_GRAIN != TOS_APP_GRAIN ||
        PLAT_STACK_BYTES != TOS_APP_STACK ||
        PLAT_HEAP_GUARD != TOS_APP_GUARD) {
        panic("board_config.h (%s) disagrees with layout.c: grain %u/%u stack %u/%u guard %u/%u",
              TOS_BOARD_NAME,
              (unsigned)TOS_APP_GRAIN, (unsigned)PLAT_APP_GRAIN,
              (unsigned)TOS_APP_STACK, (unsigned)PLAT_STACK_BYTES,
              (unsigned)TOS_APP_GUARD, (unsigned)PLAT_HEAP_GUARD);
    }
}
