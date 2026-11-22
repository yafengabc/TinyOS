#ifndef TINYOS_UACCESS_H
#define TINYOS_UACCESS_H

#include "types.h"

/* Range check for a user pointer the kernel is about to dereference.
 *
 * The flat model has no address translation, so the only thing standing
 * between a corrupt or hostile image and the kernel is this check: the loader
 * must stay inside the task window it was handed.  p_vaddr / r_offset come
 * from the file, not from the linker, so a wrapped va + len is the difference
 * between "reject the image" and "write past the window".
 *
 * Split out of kernel/elf.c so the host tests can exercise the boundary cases
 * directly instead of through a decompressor and a process table (see #53).
 * The window bounds are passed in, not stored here: two cores can exec() at
 * the same time, and a file-static pair would let one image be checked against
 * the other's window.
 *
 *   len == 0            always allowed (a zero-length write touches nothing)
 *   va < lo             outside the window
 *   va + len wraps      rejected, not wrapped
 *   va + len <= hi      inside; hi is one past the last writable byte
 */
int uaccess_range_ok(uint32_t lo, uint32_t hi, uint32_t va, uint32_t len);

#endif
