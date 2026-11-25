#include "types.h"
#include "heap.h"

/* lib.h declares sbrk() for programs; repeating the prototype here keeps this
 * file free of the rest of the userland headers, so the host test in
 * internal/build/testdata can compile it on its own with a mock sbrk. */
void *sbrk(int bytes);

/* The task-window allocator: first-fit over a free list, split on allocation,
 * merge on free, and ask sbrk() for more only when nothing on the list fits.
 *
 * The window is already the task's memory; sbrk() only moves the kernel's
 * writable boundary.  This allocator therefore never returns memory to the
 * window: free() marks and merges, the break only moves up.  That is the
 * FreeRTOS heap_4 shape, and it is what makes the heap coexist with the
 * stack-growth path and with a program that calls sbrk() itself.
 *
 * Blocks are contiguous when sbrk() hands back the address we expect.  A raw
 * sbrk() in between leaves a gap; the list stays in address order, and
 * merging checks adjacency instead of trusting it, so a gap is never merged
 * across and never handed out twice.
 *
 * Not thread-safe.  kthreads share the group's window, so two threads calling
 * malloc at the same time race here exactly as they raced on the bump pointer
 * this replaces.  kthread_create() itself only allocates before the thread
 * runs, which is safe in the same way the old "create threads first" rule was.
 */

#define HEAP_ALIGN 8u

struct heap_blk {
    uint32_t size;              /* payload bytes, header not included */
    uint32_t freed;             /* 1 while on the free list            */
    struct heap_blk *next;
    struct heap_blk *prev;
};

/* The payload must stay 8-aligned: the header is the only thing between the
 * block address and what malloc returns. */
typedef char heap_hdr_align_check[
    (sizeof(struct heap_blk) % HEAP_ALIGN) ? -1 : 1];

#define HEAP_HDR ((uint32_t)sizeof(struct heap_blk))
#define HEAP_FREE 1u

static struct heap_blk *heap_head;
static struct heap_blk *heap_tail;

#ifdef TOS_KTHREADS
/* Threads share this free list.  The lock is a kernel mutex: user mode has
 * no atomics on the M0+ or the C3, and a plain flag loses to preemption even
 * on one core.  A waiter sleeps in the kernel instead of spinning, and the
 * kernel hands ownership to the first waiter on unlock.  It is compiled only
 * when threads are possible, so single-threaded programs pay nothing. */
int kmutex_lock(int *word);
int kmutex_unlock(int *word);

static int heap_lock;

static void heap_lock_take(void)
{
    kmutex_lock(&heap_lock);
}

static void heap_unlock(void)
{
    kmutex_unlock(&heap_lock);
}
#else
#define heap_lock_take()   ((void)0)
#define heap_unlock()      ((void)0)
#endif

static int heap_adjacent(const struct heap_blk *a, const struct heap_blk *b)
{
    return (const char *)a + HEAP_HDR + a->size == (const char *)b;
}

/* Merge b into a.  a and b must be adjacent and free. */
static void heap_merge(struct heap_blk *a, struct heap_blk *b)
{
    a->size += HEAP_HDR + b->size;
    a->next = b->next;
    if (a->next)
        a->next->prev = a;
    if (heap_tail == b)
        heap_tail = a;
}

/* Give the tail of b its own free block when it is worth a header. */
static void heap_split(struct heap_blk *b, uint32_t need)
{
    uint32_t total = HEAP_HDR + b->size;
    uint32_t used = HEAP_HDR + need;
    struct heap_blk *r;

    if (total - used < HEAP_HDR + HEAP_ALIGN)
        return;
    r = (struct heap_blk *)((char *)b + used);
    r->size = total - used - HEAP_HDR;
    r->freed = HEAP_FREE;
    r->next = b->next;
    r->prev = b;
    if (r->next)
        r->next->prev = r;
    b->next = r;
    if (heap_tail == b)
        heap_tail = r;
    b->size = need;
}

/* Grow by exactly what the request needs: the free list absorbs reuse, so a
 * program that allocates and frees in a loop does not keep entering the
 * kernel, and a program that declares TOS_HEAP(n) can still malloc close to
 * n bytes. */
static struct heap_blk *heap_grow(uint32_t need)
{
    uint32_t total, ask;
    struct heap_blk *b;
    void *p;

    total = need + HEAP_HDR;
    if (total < need)
        return 0;
    ask = (total + (HEAP_ALIGN - 1u)) & ~(HEAP_ALIGN - 1u);
    if (ask < total || ask > 0x7fffffffu)
        return 0;
    p = sbrk((int)ask);
    if (!p || p == (void *)-1)
        return 0;

    b = (struct heap_blk *)p;
    b->size = ask - HEAP_HDR;
    b->freed = 0;
    b->next = 0;
    b->prev = heap_tail;
    if (heap_tail)
        heap_tail->next = b;
    else
        heap_head = b;
    heap_tail = b;
    return b;
}

void *heap_alloc(unsigned int bytes)
{
    struct heap_blk *b;
    uint32_t need;

    if (bytes == 0)
        bytes = 1;
    if (bytes > 0xffffff00u)
        return 0;
    need = (bytes + (HEAP_ALIGN - 1u)) & ~(HEAP_ALIGN - 1u);

    heap_lock_take();
    for (b = heap_head; b; b = b->next) {
        if (!b->freed || b->size < need)
            continue;
        heap_split(b, need);
        b->freed = 0;
        heap_unlock();
        return (char *)b + HEAP_HDR;
    }

    b = heap_grow(need);
    if (!b) {
        heap_unlock();
        return 0;
    }
    heap_unlock();
    return (char *)b + HEAP_HDR;
}

void heap_free(void *p)
{
    struct heap_blk *b, *n;

    if (!p)
        return;
    heap_lock_take();
    b = (struct heap_blk *)((char *)p - HEAP_HDR);
    if (b->freed) {                 /* double free: ignore, do not corrupt */
        heap_unlock();
        return;
    }
    b->freed = HEAP_FREE;

    n = b->next;
    if (n && n->freed && heap_adjacent(b, n))
        heap_merge(b, n);
    n = b->prev;
    if (n && n->freed && heap_adjacent(n, b))
        heap_merge(n, b);
    heap_unlock();
}
