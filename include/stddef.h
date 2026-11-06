#ifndef TINYOS_STD_STDDEF_SHIM_H
#define TINYOS_STD_STDDEF_SHIM_H

/* Minimal freestanding stddef.h.  The tree builds with -nostdinc, so every
 * standard header a vendored library reaches for has to exist here; this one
 * only re-exports the project's own types plus the one macro that cannot be
 * spelled without the compiler's help.
 *
 * TinyUSB pulls it in through common/tusb_common.h. */

#include "types.h"

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#ifndef __cplusplus
typedef int ptrdiff_t;
#endif

#endif
