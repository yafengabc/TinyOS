#ifndef TINYOS_KASSERT_H
#define TINYOS_KASSERT_H

#include "console.h"
#include "platform.h"

/* Kernel invariants.  TOS_KASSERT defaults on; a bring-up target can set it
 * to 0 with -DTOS_KASSERT=0.  KASSERT prints and then panics; KASSERT_HALT is
 * the ISR-safe form that cannot print.  External data must never use either:
 * it returns an error, per ARCHITECTURE-NEXT §0.3. */
#ifndef TOS_KASSERT
#define TOS_KASSERT 1
#endif

#if TOS_KASSERT
#define KASSERT(cond)                                                     \
    do {                                                                  \
        if (!(cond))                                                      \
            panic("KASSERT %s:%d: %s", __FILE__, __LINE__, #cond);        \
    } while (0)
#define KASSERT_HALT(cond)                                                \
    do {                                                                  \
        if (!(cond))                                                      \
            plat_panic_halt();                                            \
    } while (0)
#else
#define KASSERT(cond) ((void)0)
#define KASSERT_HALT(cond) ((void)0)
#endif

#endif
