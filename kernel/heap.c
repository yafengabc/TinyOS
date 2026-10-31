#include "heap.h"

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

#define HEAP_ARENA 32768         /* 32 KiB, .bss
                                    Peak use is one exec() load buffer
                                    (largest program ~2.5 KiB on RV32,
                                    ~24 KiB with the x86 programs) plus
                                    littlefs's working set. */

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

void *kmalloc(size_t size)
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

void kfree(void *ptr)
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

uint32_t heap_peak_bytes(void)
{
    return heap_peak;
}

uint32_t heap_arena_bytes(void)
{
    return HEAP_ARENA;
}
