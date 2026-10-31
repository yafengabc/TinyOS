#ifndef TINYOS_STDINT_SHIM_H
#define TINYOS_STDINT_SHIM_H

/* Minimal freestanding stdint.h: everything is already defined by the
   project's types.h (kernel/types.h).  littlefs pulls this in via
   <stdint.h> because we compile with -nostdinc. */
#include "types.h"

#endif
