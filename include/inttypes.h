#ifndef TINYOS_INTTYPES_SHIM_H
#define TINYOS_INTTYPES_SHIM_H

/* Minimal freestanding inttypes.h.  littlefs only uses the PRI macros
   inside disabled trace/error paths, so we do not need to define them. */
#include "stdint.h"

#endif
