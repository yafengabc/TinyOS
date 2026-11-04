/* py_mem.c - the interpreter's allocator.
 *
 * One arena, carved up by size class.  See py_mem.h for why this is not
 * TLSF and what question TLSF would have answered instead.
 */

#include "py_mem.h"


/* Every block carries its size, so py_free_auto() can find the class again
 * without the caller remembering what it asked for.  The header sits in
 * front of the payload, so the payload stays 8-aligned when the arena is.
 *
 * The header is 16 bytes: the size, the class, the next pointer of a free
 * block, and one word of padding.  The padding is not decoration - without
 * it the header is 12 bytes on a 32-bit target and the payload would start
 * four bytes off the alignment the rest of the interpreter assumes.
 */
typedef struct block {
    uint32_t size;     /* payload bytes the caller asked for */
    uint32_t cls;      /* size class index, or PY_NCLASS for "oversized" */
    struct block *next;
    uint32_t pad;
} block_t;

/* The header has to be a multiple of eight, so that a payload is aligned
 * the same way whatever the arena is.  Getting this wrong is not a small
 * mistake: a header that is too small writes its own fields over the first
 * bytes of the payload, and then every free() scribbles on the caller's
 * data - which shows up much later as an allocation that fails for no
 * reason. */
_Static_assert(sizeof(block_t) % 8 == 0,
               "the block header must keep the payload 8-aligned");
_Static_assert(sizeof(block_t) >= sizeof(void *) + 8,
               "a free block holds its next pointer in the payload");

#define OVERSIZE PY_NCLASS

static char *arena_base;
static char *arena_ptr;
static char *arena_end;

/* The arena is not claimed in one bite.  The interpreter's main() takes
 * a small starting chunk and the bump pointer asks for another 4 KiB
 * only when it runs out, so the break stays below the heap ceiling while
 * the script is small.
 *
 * This matters beyond memory thrift: on rv32 the kernel grows a task's
 * stack into the heap's unused tail when a store runs off the bottom
 * (process_stack_grow).  A program that pins the break at the ceiling
 * on startup makes that impossible, and the next deep expression dies
 * instead of being given another 8 KiB.  The heap and the stack are
 * meant to share this region - see the comment above process_sbrk(). */
#define PY_ARENA_CHUNK 4096u

extern void *sbrk(int bytes);

static int grow_arena(uint32_t bytes)
{
    uint32_t need = (bytes + PY_ARENA_CHUNK - 1u) & ~(PY_ARENA_CHUNK - 1u);
    char *p = (char *)sbrk((int)need);
    /* A refused sbrk leaves the break alone.  It can also succeed at an
     * address that is not where this arena ends (if somebody else moved
     * the break); the bump pointer needs one contiguous region, so that
     * is a failure too. */
    if (!p || p != arena_end)
        return 0;
    arena_end = p + need;
    return 1;
}

static block_t *freelist[PY_NCLASS];
static block_t *oversize_list;

static uint32_t stat_used;      /* bytes currently handed out */
static uint32_t stat_peak;
static uint32_t stat_live;      /* blocks in flight */

/* Round a request up to the next size class.  Classes are multiples of 16
 * bytes, so a 40-byte request becomes 48.  Classes above PY_MAX_CLASS are
 * not used: the request is served exactly, from the oversize list. */
static uint32_t class_for(uint32_t size, uint32_t *cls_out)
{
    if (size > PY_MAX_CLASS) {
        *cls_out = OVERSIZE;
        return size;
    }
    uint32_t cls = (size + PY_CLASS_STEP - 1) / PY_CLASS_STEP;
    if (cls == 0)
        cls = 1;
    *cls_out = cls;
    return cls * PY_CLASS_STEP;
}

void py_mem_init(void *base, uint32_t size)
{
    arena_base = (char *)base;
    arena_ptr = arena_base;
    arena_end = arena_base + size;
    for (int i = 0; i < PY_NCLASS; i++)
        freelist[i] = 0;
    oversize_list = 0;
    stat_used = 0;
    stat_peak = 0;
    stat_live = 0;
}

/* Take `bytes` of fresh memory from the arena.  The arena is the heap the
 * kernel reserved for this task, so this never moves the break one 4 KiB
 * step at a time the way lib.c's malloc() does. */
static void *bump(uint32_t bytes)
{
    /* Keep everything 8-aligned: the payload of every block is a pointer
     * or a struct with 64-bit fields on some targets. */
    uint32_t off = (uint32_t)(arena_ptr - arena_base);
    uint32_t pad = (8u - (off & 7u)) & 7u;
    if ((uint32_t)(arena_end - arena_ptr) < bytes + pad &&
        !grow_arena(bytes + pad))
        return 0;
    char *p = arena_ptr + pad;
    arena_ptr = p + bytes;
    return p;
}

void *py_alloc(uint32_t size)
{
    if (size == 0)
        size = 1;

    uint32_t cls;
    uint32_t rounded = class_for(size, &cls);

    block_t *b = 0;
    if (cls < PY_NCLASS) {
        if (freelist[cls]) {
            b = freelist[cls];
            freelist[cls] = b->next;
            b->next = 0;
        }
    } else {
        /* Oversized: look for an exact-size match on the oversize list
         * first, so freeing and re-allocating the same big string does not
         * grow the arena. */
        block_t **pp = &oversize_list;
        while (*pp) {
            if ((*pp)->size >= size && (*pp)->size <= size + (size >> 2)) {
                b = *pp;
                *pp = b->next;
                b->next = 0;
                break;
            }
            pp = &(*pp)->next;
        }
    }

    if (!b) {
        uint32_t need = rounded + (uint32_t)sizeof(block_t);
        void *raw = bump(need);
        if (!raw)
            return 0;
        b = (block_t *)raw;
        b->size = size;
        b->cls = cls;
        b->next = 0;
    }

    stat_used += b->size;
    if (stat_used > stat_peak)
        stat_peak = stat_used;
    stat_live++;
    return (char *)b + sizeof(block_t);
}

/* A permanent block: no header, no size class, never released.  It comes
 * out of the same arena, so the two kinds of block share the space - but
 * the permanent ones are not in any free list, so nothing can hand their
 * space to somebody else. */
void *py_alloc_perm(uint32_t size)
{
    if (size == 0)
        size = 1;
    size = (size + 7u) & ~7u;
    void *p = bump(size);
    if (!p)
        return 0;
    stat_used += size;
    if (stat_used > stat_peak)
        stat_peak = size > stat_peak ? stat_used : stat_peak;
    return p;
}

void py_free(void *ptr, uint32_t size)
{
    if (!ptr)
        return;
    block_t *b = (block_t *)((char *)ptr - sizeof(block_t));
    stat_used -= b->size;
    stat_live--;

    uint32_t cls = b->cls;
    if (cls < PY_NCLASS) {
        b->next = freelist[cls];
        freelist[cls] = b;
    } else {
        b->next = oversize_list;
        oversize_list = b;
    }
}

void py_free_auto(void *ptr)
{
    if (!ptr)
        return;
    block_t *b = (block_t *)((char *)ptr - sizeof(block_t));
    py_free(ptr, b->size);
}

uint32_t py_mem_used(void) { return stat_used; }

uint32_t py_mem_arena_used(void) { return (uint32_t)(arena_ptr - arena_base); }
uint32_t py_mem_total(void) { return (uint32_t)(arena_end - arena_base); }
uint32_t py_mem_peak(void) { return stat_peak; }
uint32_t py_mem_live_blocks(void) { return stat_live; }
