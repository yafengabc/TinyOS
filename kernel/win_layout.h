#ifndef TINYOS_WIN_LAYOUT_H
#define TINYOS_WIN_LAYOUT_H

/* Pure arithmetic for task-window placement, shared with the host harness
 * (internal/build/testdata/win_layout_harness.c).  No locks, no globals: the
 * caller owns the proc table and the interrupts.
 *
 * A window is a run of `grain`-sized steps.  Its base has to observe the
 * board's grain (every backend accepts that) and, on parts where one NAPOT
 * entry describes the stack, the stack size - layout_check() already
 * guarantees the grain is a multiple of the stack size there.
 *
 * Placement has two goals that used to be one rule:
 *
 *  1. fit - the C3 (4 KiB grain, TOR PMP) has 224 KiB free after msh and csc
 *     asks for 128 KiB.  Aligning that window to its own size needs 128 KiB
 *     of slack and fails (issue #55).  The grain is the only hard constraint.
 *  2. keep the protection map cheap - on the Hazard3 (NAPOT, seven regions)
 *     and the ARMv7-M MPU (seven regions) the number of blocks/regions a
 *     window needs depends on the base's low bits.  A 96 KiB rcc window at a
 *     64 KiB-aligned base maps as SPLIT; at a 16 KiB-aligned base it can fall
 *     to SNAP and part of .text becomes writable.
 *
 * So: try the old buddy alignment first (largest power of two not exceeding
 * the window), and fall back to the grain only when that cannot be placed.
 * C3 gets its 128 KiB window through the fallback; the NAPOT/MPU boards keep
 * the alignment that gives them the tightest map. */

/* Largest power of two not exceeding both the request and the area.  The
 * alignment the pre-#55 rule used, kept as the preferred placement. */
static inline uint32_t win_align_buddy(uint32_t bytes, uint32_t size)
{
    uint32_t cap = bytes < size ? bytes : size;
    uint32_t align = 1u;

    while (align * 2u <= cap)
        align *= 2u;
    return align;
}

static inline uint32_t win_align_grain(uint32_t grain)
{
    return (grain != 0 && (grain & (grain - 1u)) == 0) ? grain : 1u;
}

/* Largest alignment a window of `bytes` must observe in an area of `size`
 * bytes with the given grain.  Never below the grain, never above the
 * request/area, and always a power of two (align_up() below is a bit mask). */
static inline uint32_t win_align(uint32_t bytes, uint32_t size, uint32_t grain)
{
    uint32_t align = win_align_buddy(bytes, size);
    uint32_t floor = win_align_grain(grain);

    return align < floor ? floor : align;
}

/* Place a `want`-byte window at or after `cand`, ending before `next`, with
 * alignment `align`.  Returns 0 when it does not fit. */
static inline uint32_t win_place_at(uint32_t cand, uint32_t next,
                                    uint32_t want, uint32_t align)
{
    uint32_t base;

    if (align == 0 || (align & (align - 1u)) != 0)
        return 0;                       /* align_up() needs a power of two */
    base = (cand + align - 1u) & ~(align - 1u);
    if (base < cand || base >= next)    /* overflow, or aligned past the gap */
        return 0;
    return (next - base >= want) ? base : 0;
}

/* Buddy alignment first, grain as the fallback.  This is what win_alloc()
 * uses for every gap, so the C3 regression and the NAPOT map quality are one
 * rule rather than two board-specific branches. */
static inline uint32_t win_place(uint32_t cand, uint32_t next,
                                 uint32_t want, uint32_t grain)
{
    uint32_t base = win_place_at(cand, next, want,
                                 win_align_buddy(want, next - cand));

    if (base == 0)
        base = win_place_at(cand, next, want, win_align_grain(grain));
    return base;
}

#endif /* TINYOS_WIN_LAYOUT_H */
