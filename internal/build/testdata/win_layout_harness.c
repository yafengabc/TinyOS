/* Host harness for kernel/win_layout.h (issue #55).
 *
 * Two placements are pinned here:
 *
 *  1. The bug: on the ESP32-C3 the task area is 240 KiB, msh takes the first
 *     16 KiB, and csc asks for a 128 KiB window.  Aligning the base to the
 *     window size (the pre-#55 rule) can only enter the 224 KiB gap at
 *     +128 KiB, leaving 112 KiB - too small.  The 4 KiB grain is the only
 *     constraint (TOR PMP, no stack alignment rule), and the grain fallback
 *     must find the window.
 *
 *  2. The protection map: on the Hazard3 (NAPOT, seven regions) a 96 KiB rcc
 *     window at a 64 KiB-aligned base maps as SPLIT; at a 16 KiB-aligned base
 *     it can fall to SNAP, making part of .text writable.  Placement must
 *     prefer the buddy alignment and only fall back when it cannot fit.
 *
 * The harness compiles the REAL header and keeps the old alignment rule as a
 * deliberately-failing reference, so the C3 tuple proves the new code does
 * something different. */
#include <stdio.h>

typedef unsigned int uint32_t;

#include "win_layout.h"

static int failures;

static void check(int ok, const char *what, uint32_t a, uint32_t b)
{
    if (!ok) {
        printf("FAIL %s (%u, %u)\n", what, a, b);
        failures++;
    }
}

/* The pre-#55 rule from process.c: align to the largest power of two not
 * exceeding the window.  Kept so the harness proves the regression tuple
 * actually distinguishes old from new. */
static uint32_t old_align(uint32_t bytes, uint32_t size)
{
    uint32_t align = 1u;
    while (align * 2u <= bytes && align * 2u <= size)
        align *= 2u;
    return align;
}

static uint32_t place(uint32_t cand, uint32_t next, uint32_t align, uint32_t want)
{
    uint32_t base = (cand + align - 1u) & ~(align - 1u);
    if (base < cand || base >= next)
        return 0;
    return (next - base >= want) ? base : 0;
}

int main(void)
{
    /* ---- 1. C3: 128 KiB in the 224 KiB gap after msh ---- */
    const uint32_t grain = 4096u;
    const uint32_t want = 128u * 1024u;
    const uint32_t gap_start = 16u * 1024u;   /* after msh */
    const uint32_t gap_end = 240u * 1024u;    /* end of task area */
    uint32_t base;

    check(old_align(want, gap_end) == want,
          "old rule aligned to the whole window", old_align(want, gap_end), want);
    check(place(gap_start, gap_end, old_align(want, gap_end), want) == 0,
          "old rule could not place it", 0, 0);

    base = win_place(gap_start, gap_end, want, grain);
    check(base == gap_start, "grain fallback places the 128 KiB window", base, gap_start);
    check(base % grain == 0, "placed base observes the grain", base, grain);

    /* ---- 2. Hazard3: 96 KiB rcc window must keep the buddy alignment ---- */
    {
        const uint32_t g = 16u * 1024u;
        const uint32_t rcc = 96u * 1024u;
        /* A gap that starts 16 KiB past a 64 KiB boundary: the grain-aligned
         * answer (0x...4000) maps SNAP; the buddy answer (the next 64 KiB
         * boundary) maps SPLIT. */
        uint32_t cand = 0x20024000u;
        uint32_t next = 0x20060000u;
        uint32_t b = win_place(cand, next, rcc, g);

        check(b != 0, "rcc window is placed", b, 0);
        check(b % (64u * 1024u) == 0, "rcc keeps the 64 KiB alignment", b, 64u * 1024u);
        check(b == 0x20030000u, "rcc lands on the next 64 KiB boundary", b, 0x20030000u);
        /* The grain-only placement is what #55 shipped first; it is still a
         * legal window, it just costs the SPLIT map. */
        check(place(cand, next, g, rcc) == cand, "grain-only placement is worse", cand, 0);
    }

    /* ---- 3. Exact-fit gaps always place, at the gap start ---- */
    for (uint32_t g = 4096u; g <= 65536u; g *= 2u) {
        for (uint32_t mult = 1u; mult <= 8u; mult++) {
            uint32_t bytes = g * mult;
            for (uint32_t off = 0u; off <= 4u * g; off += g) {
                uint32_t a = g + off;         /* a prior window of one grain+ */
                uint32_t b = a + bytes;       /* a gap of exactly bytes */
                check(win_place(a, b, bytes, g) == a,
                      "exact-fit gap", a, bytes);
                check(win_place(a, b - g, bytes, g) == 0,
                      "short gap rejected", a, bytes);
            }
        }
    }

    /* ---- 4. Alignment is always a power of two, for align_up() ---- */
    check(win_align(16u, 16u, 4096u) == 4096u, "grain is the floor",
          win_align(16u, 16u, 4096u), 4096u);
    check(win_align(96u * 1024u, 1u << 20, 4096u) == 64u * 1024u,
          "buddy alignment preferred", win_align(96u * 1024u, 1u << 20, 4096u), 64u * 1024u);
    check(win_align(96u * 1024u, 1u << 20, 16u * 1024u) == 64u * 1024u,
          "grain does not cap a larger buddy alignment",
          win_align(96u * 1024u, 1u << 20, 16u * 1024u), 64u * 1024u);
    check(win_place_at(0, 4096u, 4096u, 3u) == 0, "non-power-of-two alignment rejected", 0, 0);

    if (failures) {
        printf("win layout: %d failures\n", failures);
        return 1;
    }
    printf("win layout: ok (C3 fallback + Hazard3 buddy alignment + sweep)\n");
    return 0;
}
