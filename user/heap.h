#ifndef TINYOS_USER_HEAP_H
#define TINYOS_USER_HEAP_H

/* The task-window allocator.
 *
 * malloc()/free() in lib.c are thin wrappers so that the rest of the userland
 * (kthread stacks, future libc pieces) can share the same free list.  The
 * unit is bytes of usable payload; block bookkeeping comes out of the heap
 * region the window declared (TOS_HEAP), the same way every RTOS heap counts
 * its headers.
 */
void *heap_alloc(unsigned int bytes);
void heap_free(void *p);

#endif
