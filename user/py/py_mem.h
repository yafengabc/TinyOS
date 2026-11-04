/* py_mem.h - memory for the Python interpreter.
 *
 * The user-space allocator cannot be used for this.  lib.c's malloc() is a
 * bump allocator: free() does nothing, and every growth moves the break by
 * at least 4096 bytes.  An interpreter that compiles a program and then runs
 * it allocates many short-lived objects (a string, a list cell, a call's
 * argument array), so it needs an allocator that actually reuses memory.
 *
 * This is not TLSF, and the difference is deliberate.
 *
 * TLSF (Two-Level Segregated Fit) answers "where does a block of this size
 * come from, without fragmenting the heap over a long run".  It is a
 * general-purpose allocator: two levels of size classes, a bitmap per level,
 * and coalescing across class boundaries.  It is the right answer for a
 * kernel or a long-running embedded process.
 *
 * The interpreter needs a different half of the problem.  Two questions
 * have to be answered separately:
 *
 *   1. Where does a block come from?      -> py_alloc, below.
 *   2. When should a block go back?        -> reference counting, in py_obj.c
 *
 * TLSF only answers (1).  Given a perfect TLSF you would still not know
 * whether a list nobody references any more should be freed - that is (2),
 * and it is the half a Python implementation actually lacks.  Bringing in
 * TLSF would have cost about 1000 lines and 4-6 KiB of flash (Matt Conte's
 * reference implementation), changed the allocator semantics of every user
 * program in the system, and still left the garbage question open.
 *
 * So: one level of size classes, each with a free list, plus a bump pointer
 * for fresh memory.  No bitmap, no second level, no coalescing across
 * classes.  Blocks are rounded up to a class, so a freed 40-byte block
 * returns to the 48-byte class and the 8 bytes above it are lost until the
 * process exits.  That is acceptable here because the interpreter's objects
 * are small and mostly the same shape, and because the process exits when
 * the script ends - the kernel hands the whole window back.
 *
 * The arena is one sbrk() of the heap the kernel already reserved for this
 * task (see TOS_HEAP in py_main.c), so the interpreter never grows the break
 * in 4 KiB steps the way malloc() does.
 */

#ifndef TINYOS_PY_MEM_H
#define TINYOS_PY_MEM_H

#include "types.h"

/* One size class per power of two from PY_MIN_CLASS up to PY_MAX_CLASS,
 * plus the classes in between, so a 40-byte request lands in 48 rather than
 * 64 and does not waste a quarter of the block.  The table is indexed by
 * (size + PY_MIN_CLASS - 1) / PY_MIN_CLASS, so classes are multiples of
 * 16 bytes: 16, 32, 48, 64, 80, ...  A class list is a singly linked list
 * of free blocks, linked through the payload itself (the payload of a free
 * block is at least PY_MIN_CLASS bytes, so it can hold the next pointer). */
#define PY_MIN_CLASS 16u
#define PY_CLASS_STEP 16u
#define PY_MAX_CLASS 1024u
#define PY_NCLASS (PY_MAX_CLASS / PY_MIN_CLASS) /* 64 */

/* Blocks larger than the biggest class get their own exact-size header and
 * are kept on a separate list; there are few of them (a long string, a big
 * list) and they are worth not rounding up.
 *
 * The header size is not a constant here: py_mem.c keeps the size and the
 * class in front of every block, and a header that disagrees with the
 * struct it describes writes over the payload.  See the static asserts in
 * py_mem.c. */

void py_mem_init(void *arena_base, uint32_t arena_size);

/* Allocate a block of at least `size` bytes.  The returned pointer is
 * 8-aligned and the block's payload is `size` bytes or more.  Returns 0
 * when the arena is exhausted; the caller must not assume it succeeded. */
void *py_alloc(uint32_t size);

/* A block that is never freed, and whose size is exactly what was asked
 * for: no header, no rounding to a size class.
 *
 * This is for what a compiled script keeps for its whole life: the token
 * array, the bytecode of each code object and the interned names.  A
 * header is 16 bytes on every block, and these are allocated once in a
 * few large pieces, so neither the header nor the rounding buys anything.
 * The arena they take is never given back because they are never released.
 *
 * The pointer is 8-aligned like every other block. */
void *py_alloc_perm(uint32_t size);

/* Return a block to its class.  `size` must be the size that was asked
 * for, because that is what decides the class; the header keeps it, so
 * callers normally use py_free() instead of calling this directly. */
void py_free(void *ptr, uint32_t size);

/* The convenience form: the size comes from the block's own header. */
void py_free_auto(void *ptr);

/* Statistics, so the interpreter can say what it used instead of leaving
 * the reader to guess. */
uint32_t py_mem_used(void);

/* The bytes the arena has actually handed out, headers and rounding
 * included.  py_mem_used() counts what the callers asked for, which is
 * less: a block that is reused for a smaller request still occupies the
 * room of the bigger one, and every block has a header.  The difference
 * between the two is the price of the bookkeeping. */
uint32_t py_mem_arena_used(void);
uint32_t py_mem_total(void);
uint32_t py_mem_peak(void);
uint32_t py_mem_live_blocks(void);

#endif
