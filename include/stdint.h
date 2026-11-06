#ifndef TINYOS_STDINT_SHIM_H
#define TINYOS_STDINT_SHIM_H

/* Minimal freestanding stdint.h: the types themselves come from the
   project's types.h (kernel/types.h).  littlefs pulls this in via
   <stdint.h> because we compile with -nostdinc, and TinyUSB needs the limit
   macros (UINT32_MAX and friends) that a real stdint.h also provides. */
#include "types.h"

#ifndef INT8_MAX
#define INT8_MAX   127
#define INT8_MIN   (-128)
#define UINT8_MAX  255u
#endif

#ifndef INT16_MAX
#define INT16_MAX  32767
#define INT16_MIN  (-32768)
#define UINT16_MAX 65535u
#endif

#ifndef INT32_MAX
#define INT32_MAX  2147483647
#define INT32_MIN  (-2147483647 - 1)
#define UINT32_MAX 4294967295u
#endif

#ifndef INT64_MAX
#define INT64_MAX  9223372036854775807LL
#define INT64_MIN  (-9223372036854775807LL - 1)
#define UINT64_MAX 18446744073709551615ULL
#endif

/* Pointer-sized limits: every architecture this tree builds for has 32-bit
   pointers (i386, rv32, Cortex-M). */
#ifndef INTPTR_MAX
#define INTPTR_MAX  INT32_MAX
#define UINTPTR_MAX UINT32_MAX
#endif

#endif
