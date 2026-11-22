#ifndef TINYOS_UACCESS_H
#define TINYOS_UACCESS_H

#include "types.h"

/* True when [va, va+len) is wholly inside [lo, hi).  len == 0 is allowed at
 * any address.  The flat model has no page tables, so this is the only check
 * between an untrusted ELF/exec envelope and a raw store into the task
 * window.  Kept here rather than static in elf.c so host tests can compile
 * the real implementation without the loader around it. */
int uaccess_range_ok(uint32_t lo, uint32_t hi, uint32_t va, uint32_t len);

#endif /* TINYOS_UACCESS_H */
