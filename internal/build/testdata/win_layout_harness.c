/* Host harness for kernel/win_layout.h (issue #55).
 *
 * The bug: on the ESP32-C3 the task area is 240 KiB, msh takes the first
 * 16 KiB, and csc asks for a 128 KiB window.  The old placement aligned the
 * base to the window size (128 KiB), so the 224 KiB gap starting at +16 KiB
 * could only be entered at +128 KiB - leaving 112 KiB, too small.  The board
 * has a 4 KiB grain and TOR PMP (no stack alignment rule), so 4 KiB is the
 * only constraint that matters.
 *
 * The harness compiles the REAL header and checks both the regression tuple
 * and a sweep over grains/gap positions, including the old behaviour as a
 * deliberately-failing reference for the negative case. */
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

/* The old rule from process.c before #55: align to the largest power of two
 * not exceeding the window.  Kept here so the harness proves the regression
 * tuple actually distinguishes old from new. */
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
    const uint32_t grain = 4096u;
    const uint32_t want = 128u * 1024u;
    const uint32_t gap_start = 16u * 1024u;   /* after msh */
    const uint32_t gap_end = 240u * 1024u;    /* end of task area */
    uint32_t base;

    /* The regression: new rule fits, old rule does not. */
    check(win_align(want, gap_end - grain, grain) == grain,
          "C3 align is the 4 KiB grain",
          win_align(want, gap_end - grain, grain), grain);
    base = place(gap_start, gap_end, win_align(want, gap_end, grain), want);
    check(base != 0, "128 KiB window fits the 224 KiB gap", base, want);
    check(old_align(want, gap_end - grain) == want,
          "old rule aligned to the whole window", old_align(want, gap_end), want);
    check(place(gap_start, gap_end, old_align(want, gap_end), want) == 0,
          "old rule could not place it", 0, 0);

    /* Sweep: every grain-multiple window must fit a gap of exactly that
     * size, wherever the gap starts. */
    for (uint32_t g = 4096u; g <= 65536u; g *= 2u) {
        for (uint32_t mult = 1u; mult <= 8u; mult++) {
            uint32_t bytes = g * mult;
            /* cand is always grain-aligned: every prior window end is a
             * grain multiple, and dom->base is too (layout_check()). */
            for (uint32_t off = 0u; off <= 4u * g; off += g) {
                uint32_t a = g + off;         /* a prior window of one grain+ */
                uint32_t b = a + bytes;       /* a gap of exactly bytes */
                check(win_gap_fits(a, b, bytes, g) == bytes,
                      "exact-fit gap", a, bytes);
                /* One grain short must never fit. */
                check(win_gap_fits(a, b - g, bytes, g) == 0,
                      "short gap rejected", a, bytes);
            }
        }
    }

    /* Clamping: a grain larger than the request or the area must not push
     * the base out of bounds. */
    check(win_align(16u, 16u, 4096u) == 16u, "align capped at request",
          win_align(16u, 16u, 4096u), 16u);
    check(win_align(4096u, 32u, 4096u) == 32u, "align capped at area",
          win_align(4096u, 32u, 4096u), 32u);

    if (failures) {
        printf("win layout: %d failures\n", failures);
        return 1;
    }
    printf("win layout: ok (C3 128 KiB regression + grain sweep)\n");
    return 0;
}
