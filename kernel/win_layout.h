#ifndef TINYOS_WIN_LAYOUT_H
#define TINYOS_WIN_LAYOUT_H

/* Pure arithmetic for task-window placement, shared with the host harness
 * (internal/build/testdata/win_layout_harness.c).  No locks, no globals: the
 * caller owns the proc table and the interrupts.
 *
 * A window is a run of `grain`-sized steps, not a buddy block.  Its base has
 * to satisfy the board's grain (every backend accepts that) and, on parts
 * where one NAPOT entry describes the stack, the stack size.  The old code
 * aligned to the largest power of two not exceeding the whole window, which
 * for a 128 KiB window on the C3 (4 KiB grain, TOR PMP, no stack alignment
 * requirement) wasted up to 112 KiB and made a 128 KiB window fail in a
 * 224 KiB gap - issue #55. */

/* Largest alignment a window of `bytes` must observe in an area of `size`
 * bytes with the given grain.  The grain is the contract: all backends
 * describe windows as whole grains, and layout_check() already guarantees
 * grain is a multiple of the stack size on the parts that need it. */
static inline uint32_t win_align(uint32_t bytes, uint32_t size, uint32_t grain)
{
    uint32_t align = (grain != 0) ? grain : 1u;

    /* The grain is the ceiling, not the floor: a 128 KiB window on a 4 KiB
     * grain board may start on any 4 KiB step.  Do not grow to the window
     * size - that was the old bug (up to 112 KiB wasted in a 224 KiB gap).
     * Still cap at the request/area so a grain larger than both cannot push
     * the base past the gap. */
    if (align > bytes)
        align = bytes;
    if (align > size)
        align = size;
    return align ? align : 1u;
}

/* Largest window of `want` bytes that can start at `cand` and end before
 * `next`, after aligning the base.  Returns 0 when it does not fit. */
static inline uint32_t win_gap_fits(uint32_t cand, uint32_t next,
                                    uint32_t want, uint32_t grain)
{
    uint32_t align = win_align(want, next - cand, grain);
    uint32_t base = (cand + align - 1u) & ~(align - 1u);

    if (base < cand || base >= next)  /* overflow or aligned past the gap */
        return 0;
    return (next - base >= want) ? want : 0;
}

#endif /* TINYOS_WIN_LAYOUT_H */
