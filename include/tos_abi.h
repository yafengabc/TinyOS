#ifndef TINYOS_TOS_ABI_H
#define TINYOS_TOS_ABI_H

/* ABI-owned data structures.
 *
 * These definitions live outside user/lib.h because the kernel writes them
 * field by field through user pointers.  A change here is an ABI change:
 * append fields only, never reorder or resize an existing one.  The kernel
 * includes this header too and uses sizeof/offsetof below, so a layout drift
 * is a compile error instead of a silent pointer walk.
 *
 * TOS_ABI_VERSION is the structural version used by A-2.  It is not the
 * LZ4TOS image-format version (that is C2/B-3).
 */

#include <stddef.h>
#include <stdint.h>

#define TOS_ABI_VERSION 1

struct stat {
    uint32_t size;   /* file size in bytes (0 for directories/devices) */
    uint8_t  type;   /* TOS_FTYPE_* */
};

struct meminfo {
    uint32_t total;        /* heap arena size in bytes        */
    uint32_t used;         /* currently allocated             */
    uint32_t free;         /* total - used                   */
    uint32_t peak;         /* peak bytes ever allocated       */
    uint32_t ram_total;    /* usable RAM from the kernel base */
    uint32_t kernel_used;  /* kernel image + .bss, heap included */
    uint32_t app_total;    /* RAM reserved for task windows   */
    uint32_t ram_unused;   /* ram_total - kernel_used - app_total */
};

struct dfinfo {
    uint32_t total;   /* littlefs partition size in bytes */
    uint32_t used;    /* bytes consumed by littlefs        */
};

struct clockinfo {
    uint64_t ns;       /* monotonic nanoseconds since boot */
    uint32_t us;       /* same instant, microseconds (low 32 bits) */
    uint32_t ms;       /* same instant, milliseconds (low 32 bits) */
    uint32_t tick_hz;  /* scheduler tick rate             */
    uint32_t cpus;     /* CPUs in this build              */
};

/* The offsets are the contract used by kernel/syscall.c.  Keep these in
 * sync with the field order above; this is deliberately a compile-time
 * failure, not a runtime check. */
_Static_assert(sizeof(struct stat) == 8, "struct stat ABI size");
_Static_assert(offsetof(struct stat, size) == 0, "struct stat.size ABI");
_Static_assert(offsetof(struct stat, type) == 4, "struct stat.type ABI");

_Static_assert(sizeof(struct meminfo) == 32, "struct meminfo ABI size");
_Static_assert(offsetof(struct meminfo, total) == 0, "meminfo.total ABI");
_Static_assert(offsetof(struct meminfo, used) == 4, "meminfo.used ABI");
_Static_assert(offsetof(struct meminfo, free) == 8, "meminfo.free ABI");
_Static_assert(offsetof(struct meminfo, peak) == 12, "meminfo.peak ABI");
_Static_assert(offsetof(struct meminfo, ram_total) == 16, "meminfo.ram_total ABI");
_Static_assert(offsetof(struct meminfo, kernel_used) == 20, "meminfo.kernel_used ABI");
_Static_assert(offsetof(struct meminfo, app_total) == 24, "meminfo.app_total ABI");
_Static_assert(offsetof(struct meminfo, ram_unused) == 28, "meminfo.ram_unused ABI");

_Static_assert(sizeof(struct dfinfo) == 8, "struct dfinfo ABI size");
_Static_assert(offsetof(struct dfinfo, total) == 0, "dfinfo.total ABI");
_Static_assert(offsetof(struct dfinfo, used) == 4, "dfinfo.used ABI");

_Static_assert(sizeof(struct clockinfo) == 24, "struct clockinfo ABI size");
_Static_assert(offsetof(struct clockinfo, ns) == 0, "clockinfo.ns ABI");
_Static_assert(offsetof(struct clockinfo, us) == 8, "clockinfo.us ABI");
_Static_assert(offsetof(struct clockinfo, ms) == 12, "clockinfo.ms ABI");
_Static_assert(offsetof(struct clockinfo, tick_hz) == 16, "clockinfo.tick_hz ABI");
_Static_assert(offsetof(struct clockinfo, cpus) == 20, "clockinfo.cpus ABI");

#endif
