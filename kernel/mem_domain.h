#ifndef TINYOS_MEM_DOMAIN_H
#define TINYOS_MEM_DOMAIN_H

#include "types.h"

/* Memory domains (C4).  Today there is exactly one executable domain, built
 * from the linker's __app_base and the platform's PLAT_APP_* constants; the
 * struct exists so the allocator and the protection checks take a domain
 * instead of reading globals, and so a second (data-only) domain can be
 * appended without touching the window allocator's scan.
 *
 * flags is a type, not documentation: win_alloc() only accepts EXEC, so
 * "PSRAM must not be executable" can be enforced by construction later. */

#define DOMAIN_EXEC 0x1u
#define DOMAIN_DATA 0x2u

struct mem_domain {
    const char *name;
    uint32_t base;
    uint32_t size;
    uint32_t grain;
    unsigned flags;
};

/* Fill the single executable domain from the linker/platform constants.
 * Called once from kmain before process_init(); idempotent. */
void mem_domain_init(void);

/* The executable domain windows are allocated from.  NULL before init. */
const struct mem_domain *mem_domain_exec(void);

#endif
