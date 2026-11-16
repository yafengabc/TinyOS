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

/* Sizes are cheap enough for every compiler (goc32 included).  Field
 * offsets need offsetof(); goc32's front end cannot fold that, so the
 * offset asserts live in tos_abi_asserts.h and are included by the kernel
 * only.  Both files are generated from the same struct definitions, so a
 * field reorder still fails the kernel build. */
_Static_assert(sizeof(struct stat) == 8, "struct stat ABI size");
_Static_assert(sizeof(struct meminfo) == 32, "struct meminfo ABI size");
_Static_assert(sizeof(struct dfinfo) == 8, "struct dfinfo ABI size");
_Static_assert(sizeof(struct clockinfo) == 24, "struct clockinfo ABI size");

#endif
