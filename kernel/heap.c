#include "heap.h"
#include "cpu.h"

/* First-fit allocator over a static arena with free-block coalescing.
   The kernel's .bss is cleared by entry.S, so the arena starts zeroed;
   heap_init() links it into one free block.

   The free list is kept sorted by address so kfree() can merge adjacent
   free blocks.  littlefs allocates and frees many small buffers (caches,
   per-file buffers) while writing, which fragments the arena quickly if
   coalescing is missing.

   Block layout (16 byte header):
     [size | next | pad]
   The low bit of `size` marks an allocated block. */

/* The arena is .bss, so its size is a board decision: the build passes
   -DTOS_HEAP_BYTES for boards that say something other than the 32 KiB
   default (the C3 dropped to 16 KiB once exec() stopped loading images
   through the heap).  Peak use is littlefs's working set plus the small
   per-open-file caches - exec() decompresses straight into the task window
   and never allocates a buffer proportional to the program size. */
#ifndef TOS_HEAP_BYTES
#define TOS_HEAP_BYTES 32768     /* 32 KiB for boards that say nothing */
#endif
#define HEAP_ARENA ((uint32_t)TOS_HEAP_BYTES)

static uint8_t arena[HEAP_ARENA] __attribute__((aligned(16)));

static uint32_t heap_used;
static uint32_t heap_peak;

typedef struct blk {
    uint32_t    size;            /* payload size | allocated bit */
    struct blk *next;            /* only meaningful while free */
    uint8_t     pad[8];          /* keep payload 8-byte aligned */
} blk_t;

#define ALLOC_BIT 0x80000000u
#define BLK_HEADER ((uint32_t)sizeof(blk_t))

static blk_t *free_head;
static int    init_done;

static blk_t *header_of(void *payload)
{
    return (blk_t *)((uint8_t *)payload - BLK_HEADER);
}

void heap_init(void)
{
    if (init_done)
        return;
    free_head = (blk_t *)arena;
    free_head->size = HEAP_ARENA - BLK_HEADER;   /* free, no alloc bit */
    free_head->next = NULL;
    init_done = 1;
}

static void *kmalloc_locked(size_t size)
{
    heap_init();
    if (size == 0)
        size = 1;
    size = (size + 7u) & ~7u;

    blk_t *prev = NULL;
    for (blk_t *b = free_head; b; prev = b, b = b->next) {
        if (b->size < size)
            continue;

        if (b->size >= size + BLK_HEADER + 8) {
            /* Split: keep the remainder as a free block at b+size. */
            uint32_t remain = b->size - size - BLK_HEADER;
            blk_t *rest = (blk_t *)((uint8_t *)b + BLK_HEADER + size);
            rest->size = remain;
            rest->next = b->next;   /* insert before the old successor */
            b->size = size | ALLOC_BIT;
            if (prev)
                prev->next = rest;
            else
                free_head = rest;
        } else {
            b->size |= ALLOC_BIT;
            if (prev)
                prev->next = b->next;
            else
                free_head = b->next;
        }
        heap_used += b->size & ~ALLOC_BIT;
        if (heap_used > heap_peak)
            heap_peak = heap_used;
        return (void *)((uint8_t *)b + BLK_HEADER);
    }
    return NULL;   /* out of heap */
}

/* Insert a free block into the address-sorted free list and coalesce
   with any adjacent free neighbours. */
static void free_insert(blk_t *b)
{
    uint8_t *addr = (uint8_t *)b;
    uint8_t *end = addr + BLK_HEADER + (b->size & ~ALLOC_BIT);

    blk_t *prev = NULL;
    blk_t *cur = free_head;
    while (cur && (uint8_t *)cur < addr) {
        prev = cur;
        cur = cur->next;
    }

    /* merge with successor if it starts right after this block */
    if (cur && (uint8_t *)cur == end) {
        b->size = (b->size & ~ALLOC_BIT) + BLK_HEADER + (cur->size & ~ALLOC_BIT);
        b->next = cur->next;
        cur = b->next;
    } else {
        b->next = cur;
    }

    /* merge with predecessor if this block starts right after it */
    if (prev && (uint8_t *)prev + BLK_HEADER + (prev->size & ~ALLOC_BIT) == addr) {
        prev->size = (prev->size & ~ALLOC_BIT) + BLK_HEADER + (b->size & ~ALLOC_BIT);
        prev->next = b->next;
        return;
    }

    if (prev)
        prev->next = b;
    else
        free_head = b;
}

static void kfree_locked(void *ptr)
{
    if (!ptr)
        return;
    blk_t *b = header_of(ptr);
    if (!(b->size & ALLOC_BIT))
        return;

    heap_used -= b->size & ~ALLOC_BIT;
    b->size &= ~ALLOC_BIT;
    free_insert(b);
}

/* ---- locking ---------------------------------------------------------- *
 *
 * The arena is one free list, and on an SMP build two CPUs really can be in
 * it at once: an exec() on one hart and a /proc open() on the other is
 * enough.  Two interleaved first-fit walks hand out the same block twice,
 * and that does not fail loudly - it silently corrupts whoever got the block
 * first.
 *
 * Interrupts are not disabled around the lock because no handler allocates:
 * the tty queues are static arrays and the timer path only touches the
 * process table.  If that ever changes, this becomes irqsave + spin.
 */
static spin_t heap_lock = SPIN_INIT;

void *kmalloc(size_t size)
{
    void *p;

    spin_lock(&heap_lock);
    p = kmalloc_locked(size);
    spin_unlock(&heap_lock);
    return p;
}

void kfree(void *ptr)
{
    spin_lock(&heap_lock);
    kfree_locked(ptr);
    spin_unlock(&heap_lock);
}

uint32_t heap_peak_bytes(void)
{
    return heap_peak;
}

uint32_t heap_arena_bytes(void)
{
    return HEAP_ARENA;
}

uint32_t heap_used_bytes(void)
{
    return heap_used;
}
