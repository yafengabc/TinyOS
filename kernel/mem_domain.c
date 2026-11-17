#include "mem_domain.h"
#include "platform.h"

/* One executable domain.  It is a view over PLAT_APP_*, not a second truth:
 * board_check.c compares the two at boot, and the generator cross-checks the
 * same numbers against board.conf (B-1b).  When P4 grows a PSRAM data domain,
 * it is appended here with DOMAIN_DATA and never handed to win_alloc(). */
static struct mem_domain app_domain;

void mem_domain_init(void)
{
    app_domain.name  = "app";
    app_domain.base  = PLAT_APP_BASE;
    app_domain.size  = PLAT_APP_MAX - PLAT_APP_BASE;
    app_domain.grain = PLAT_APP_GRAIN;
    app_domain.flags = DOMAIN_EXEC;
}

const struct mem_domain *mem_domain_exec(void)
{
    return &app_domain;
}
