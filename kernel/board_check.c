#include "platform.h"
#include "console.h"
#include "board_config.h"
#include "mem_domain.h"

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
    const struct mem_domain *d = mem_domain_exec();

    if (PLAT_APP_GRAIN != TOS_APP_GRAIN ||
        PLAT_STACK_BYTES != TOS_APP_STACK ||
        PLAT_HEAP_GUARD != TOS_APP_GUARD ||
        PLAT_STACK_AREA != TOS_APP_STACK_AREA) {
        panic("board_config.h (%s) disagrees with layout.c: grain %u/%u stack %u/%u guard %u/%u area %u/%u",
              TOS_BOARD_NAME,
              (unsigned)TOS_APP_GRAIN, (unsigned)PLAT_APP_GRAIN,
              (unsigned)TOS_APP_STACK, (unsigned)PLAT_STACK_BYTES,
              (unsigned)TOS_APP_GUARD, (unsigned)PLAT_HEAP_GUARD,
              (unsigned)TOS_APP_STACK_AREA, (unsigned)PLAT_STACK_AREA);
    }
    /* C4 seam: the allocator's domain is a view over the same layout. */
    if (!d || d->base != PLAT_APP_BASE ||
        d->size != PLAT_APP_MAX - PLAT_APP_BASE ||
        d->grain != PLAT_APP_GRAIN ||
        !(d->flags & DOMAIN_EXEC)) {
        panic("mem_domain disagrees with platform layout");
    }
#if TOS_HAS_FLASH
    /* The flash geometry has the same two declarations as the window numbers:
     * board.conf says how big the chip is, layout.c derives the partition from
     * it.  A board built against the wrong part would mount a filesystem that
     * runs past the end of the chip, so refuse to boot instead. */
    if (PLAT_LFS_LBA != TOS_LFS_OFFSET / 512u ||
        PLAT_LFS_BLOCKS != TOS_LFS_BLOCKS) {
        panic("board_config.h (%s) disagrees with layout.c: lfs lba %u/%u blocks %u/%u",
              TOS_BOARD_NAME,
              (unsigned)(TOS_LFS_OFFSET / 512u), (unsigned)PLAT_LFS_LBA,
              (unsigned)TOS_LFS_BLOCKS, (unsigned)PLAT_LFS_BLOCKS);
    }
#endif
}
