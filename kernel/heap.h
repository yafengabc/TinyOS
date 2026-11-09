#ifndef TINYOS_HEAP_H
#define TINYOS_HEAP_H

#include "types.h"

/* Tiny kernel heap.  littlefs (third_party/littlefs) allocates its caches
   and per-file buffers through kmalloc/kfree. */
void heap_init(void);
void *kmalloc(size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);

/* Diagnostics: peak bytes ever allocated and the arena size, so the boot
   log can show how much is really needed. */
uint32_t heap_peak_bytes(void);
uint32_t heap_arena_bytes(void);
uint32_t heap_used_bytes(void);

#endif
