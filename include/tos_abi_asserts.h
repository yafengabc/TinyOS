#ifndef TINYOS_TOS_ABI_ASSERTS_H
#define TINYOS_TOS_ABI_ASSERTS_H

/* Field-offset contract for the structures kernel/syscall.c writes through
 * user pointers.
 *
 * Kept out of tos_abi.h on purpose: goc32's front end cannot fold
 * __builtin_offsetof, and every user program includes tos_abi.h via
 * user/lib.h.  Only the kernel includes this file, so clang still fails the
 * kernel build on a reorder while the goc32 sample compile keeps working.
 * Add a new field's assert here in the same commit that adds the field. */

#include "tos_abi.h"

_Static_assert(offsetof(struct stat, size) == 0, "struct stat.size ABI");
_Static_assert(offsetof(struct stat, type) == 4, "struct stat.type ABI");

_Static_assert(offsetof(struct meminfo, total) == 0, "meminfo.total ABI");
_Static_assert(offsetof(struct meminfo, used) == 4, "meminfo.used ABI");
_Static_assert(offsetof(struct meminfo, free) == 8, "meminfo.free ABI");
_Static_assert(offsetof(struct meminfo, peak) == 12, "meminfo.peak ABI");
_Static_assert(offsetof(struct meminfo, ram_total) == 16, "meminfo.ram_total ABI");
_Static_assert(offsetof(struct meminfo, kernel_used) == 20, "meminfo.kernel_used ABI");
_Static_assert(offsetof(struct meminfo, app_total) == 24, "meminfo.app_total ABI");
_Static_assert(offsetof(struct meminfo, ram_unused) == 28, "meminfo.ram_unused ABI");

_Static_assert(offsetof(struct dfinfo, total) == 0, "dfinfo.total ABI");
_Static_assert(offsetof(struct dfinfo, used) == 4, "dfinfo.used ABI");

_Static_assert(offsetof(struct clockinfo, ns) == 0, "clockinfo.ns ABI");
_Static_assert(offsetof(struct clockinfo, us) == 8, "clockinfo.us ABI");
_Static_assert(offsetof(struct clockinfo, ms) == 12, "clockinfo.ms ABI");
_Static_assert(offsetof(struct clockinfo, tick_hz) == 16, "clockinfo.tick_hz ABI");
_Static_assert(offsetof(struct clockinfo, cpus) == 20, "clockinfo.cpus ABI");

#endif
