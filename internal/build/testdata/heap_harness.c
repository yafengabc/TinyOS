/* Host harness for the task-window heap (user/heap.c).
 *
 * The allocator is compiled with -Dsbrk=heap_test_sbrk, so this file owns the
 * break the same way the kernel does for a task: sbrk() hands out the next
 * bytes of a fixed arena and returns the old break.  That makes every
 * behaviour a board would show - reuse, split, merge, growth, a raw sbrk()
 * leaving a gap, refusal at the end of the window - testable without a board.
 *
 * The Go side (internal/build/heap_test.go) compiles this against the real
 * heap.c and requires exit status 0 plus "ok" in the output. */
#include <stdint.h>
#include <stdio.h>

void *heap_alloc(unsigned int bytes);
void heap_free(void *p);
void *heap_test_sbrk(int bytes);

/* 8-byte aligned so the block header's pointers are valid host pointers. */
static uint64_t arena_words[256 * 1024 / 8];
#define ARENA ((unsigned char *)arena_words)
static unsigned int brk_off;
static int failures;

void *heap_test_sbrk(int bytes)
{
    unsigned int old = brk_off;

    if (bytes < 0)
        return 0;                   /* the heap never shrinks */
    if (bytes == 0)
        return ARENA + old;
    if ((unsigned int)bytes > sizeof(arena_words) - old)
        return 0;
    brk_off = old + (unsigned int)bytes;
    return ARENA + old;
}

static void check(int ok, const char *what)
{
    if (!ok) {
        printf("FAIL %s (brk=%u)\n", what, brk_off);
        failures++;
    }
}

int main(void)
{
    void *g, *a, *b, *c, *x, *y, *z, *w, *raw, *q, *s;
    unsigned int t;

    /* Growth: the first request has nothing to reuse. */
    g = heap_alloc(60000);
    check(g != 0, "large allocation");
    check(brk_off >= 60016, "large allocation grew the break");
    check(((uintptr_t)g & 7u) == 0, "large allocation is 8-aligned");

    a = heap_alloc(1);
    b = heap_alloc(7);
    check(a != 0 && b != 0 && a != b, "two small allocations");
    check(((uintptr_t)a & 7u) == 0 && ((uintptr_t)b & 7u) == 0,
          "small allocations are 8-aligned");

    /* The tail block must be reusable without touching the break. */
    t = brk_off;
    heap_free(b);
    c = heap_alloc(7);
    check(c == b, "freed tail reused at the same address");
    check(brk_off == t, "reuse did not grow the break");

    /* A raw sbrk() between allocations leaves a gap; the next block goes
     * above it and the gap is never merged across. */
    raw = heap_test_sbrk(4096);
    check(raw != 0, "raw sbrk");
    q = heap_alloc(64);
    check(q != 0, "allocation after a raw sbrk");
    check((unsigned char *)q >= (unsigned char *)raw + 4096,
          "new block starts above the raw gap");

    /* Split: one 4 KiB block serves two 1000-byte requests. */
    x = heap_alloc(4096);
    check(x != 0, "4 KiB allocation");
    t = brk_off;
    heap_free(x);
    y = heap_alloc(1000);
    z = heap_alloc(1000);
    check(y == x, "split reuses the freed block from its start");
    check(z != 0 && brk_off == t, "second half came from the split");

    /* Merge: freeing both halves makes one block that serves a 3000-byte
     * request without a grow. */
    heap_free(y);
    heap_free(z);
    t = brk_off;
    w = heap_alloc(3000);
    check(w == y, "merged block reused from its start");
    check(brk_off == t, "merge avoided a grow");

    /* The end of the window is a hard stop, not a crash. */
    check(heap_alloc(1u << 20) == 0, "oversized request refused");

    /* Double free is ignored, and it does not poison the free list. */
    heap_free(q);
    heap_free(q);
    s = heap_alloc(64);
    check(s != 0, "allocator still works after a double free");

    /* free() never moves the break down. */
    t = brk_off;
    heap_free(s);
    check(brk_off == t, "free never shrinks the break");

    heap_free(0);                   /* free(NULL) is a no-op */

    if (failures)
        return 1;
    printf("ok (%u bytes of arena used)\n", brk_off);
    return 0;
}
