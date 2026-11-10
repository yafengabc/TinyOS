#ifndef TINYOS_LFS_CONFIG_H
#define TINYOS_LFS_CONFIG_H

#include "types.h"

/* Configuration for the vendored littlefs in third_party/littlefs.
   This header is force-included (-include) when compiling lfs.c and
   lfs_util.c so the LFS_* macros are set before lfs_util.h is parsed. */

/* Strip all logging / assertions: we have no stdio in the kernel. */
#define LFS_NO_DEBUG
#define LFS_NO_WARN
#define LFS_NO_ERROR
#define LFS_NO_TRACE
#define LFS_NO_ASSERT

/* A read-only appliance build: littlefs drops its allocation, file-write and
 * remove paths, and ramdisk.c stubs the matching entry points. */
#ifdef TOS_LFS_RO
#define LFS_READONLY
#endif

/* No libc malloc.  Route littlefs allocations through the kernel heap.
   LFS_NO_MALLOC skips <stdlib.h>; defining LFS_MALLOC/LFS_FREE makes the
   inline lfs_malloc/lfs_free wrappers call our allocator instead. */
#define LFS_NO_MALLOC
#define LFS_MALLOC(size) kmalloc(size)
#define LFS_FREE(ptr)    kfree(ptr)

void *kmalloc(size_t size);
void  kfree(void *ptr);

#endif
