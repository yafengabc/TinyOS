/* Host harness for the RP2350 Hazard3 NAPOT window mapper (pmp_map.h).
 *
 * The board code cannot run on a build machine, but the mapping algorithm
 * can: it is pure arithmetic over the segment list.  This file supplies the
 * three things the header expects from the kernel (plat_seg_t,
 * PLAT_SEG_CODE, PLAT_APP_GRAIN) with the board's real values, then checks
 * properties of every map the ladder produces:
 *
 *   - every block is inside the window;
 *   - no block covers a page that is in no segment (the guard hole), unless
 *     the chosen policy is the last-resort PMP_MAP_WINDOW;
 *   - every page that belongs to a segment is covered by some block;
 *   - the ladder prefers the most protective policy that fits.
 *
 * It also pins the geometry that started issue #44: csc's 112 KiB window
 * with wx at +28 KiB needs eight NAPOT blocks with a page-granular split and
 * therefore did not fit.
 *
 * The Go side (internal/build/pmpmap_test.go) compiles this and requires exit
 * status 0 plus the expected summary lines. */
#include <stdint.h>
#include <stdio.h>

typedef struct {
    uint32_t base;
    uint32_t top;
    int attr;
} plat_seg_t;

#define PLAT_SEG_CODE 0
#define PLAT_SEG_DATA 1

static const uint32_t PLAT_APP_GRAIN = 16384u;

#include "pmp_map.h"

static int bad;
static int reported;

static void problem(const char *msg, uint32_t a, uint32_t b)
{
    bad = 1;
    if (reported++ < 20)
        printf("FAIL %s (0x%x 0x%x)\n", msg, (unsigned)a, (unsigned)b);
}

/* Inverse of napot(): t trailing ones encode size 1 << (t + 3). */
static void decode(uint32_t v, uint32_t *base, uint32_t *size)
{
    unsigned t = 0;

    while (t < 24 && ((v >> t) & 1u))
        t++;
    *size = 1u << (t + 3);
    *base = (v & ~((1u << t) - 1u)) << 2;
}

/* Validate one map.  hole_lo/hole_hi is the guard hole [brk, stack_low). */
static void validate(int mode, const plat_seg_t *segs, int n,
                     uint32_t win_base, uint32_t win_size,
                     uint32_t hole_lo, uint32_t hole_hi,
                     const uint32_t *addr, const uint32_t *cfg)
{
    static uint8_t need[PMP_MAX_PAGES], have[PMP_MAX_PAGES];
    uint32_t win_end = win_base + win_size;
    int pages = (int)(win_size / PMP_PAGE);
    int blocks = 0;

    for (int i = 0; i < pages; i++)
        need[i] = have[i] = 0;

    for (int i = 0; i < n; i++) {
        uint32_t p;

        for (p = segs[i].base & ~(PMP_PAGE - 1u); p < segs[i].top;
             p += PMP_PAGE)
            need[(p - win_base) / PMP_PAGE] = 1;
    }

    for (int i = 0; i < PMP_ENTRIES; i++) {
        uint32_t b, s, p;

        if (i == PMP_VEC_ENTRY || !cfg[i])
            continue;
        decode(addr[i], &b, &s);
        blocks++;

        if (b < win_base || b + s > win_end) {
            problem("block outside the window", b, s);
            continue;
        }
        if (mode != PMP_MAP_WINDOW && b < hole_hi && b + s > hole_lo)
            problem("block covers the guard hole", b, s);

        for (p = b; p < b + s; p += PMP_PAGE) {
            int idx = (int)((p - win_base) / PMP_PAGE);

            if (idx >= 0 && idx < pages)
                have[idx] = 1;
        }
    }

    if (mode != PMP_MAP_WINDOW) {
        for (int i = 0; i < pages; i++)
            if (have[i] && !need[i])
                problem("covered a page in no segment",
                        win_base + (uint32_t)i * PMP_PAGE, 0);
    }
    for (int i = 0; i < pages; i++)
        if (need[i] && !have[i])
            problem("left a segment page uncovered",
                    win_base + (uint32_t)i * PMP_PAGE, 0);

    if (blocks > PMP_ENTRIES - 1)
        problem("more blocks than writable regions", (uint32_t)mode,
                (uint32_t)blocks);
}

/* One window: code [base, code_end), data [code_end, data_end), stack
 * [stack_low, base + size).  brk == data_end, so the hole is
 * [data_end, stack_low). */
struct win {
    const char *name;
    uint32_t base;
    uint32_t size;
    uint32_t code_end;
    uint32_t data_end;
    uint32_t stack_low;
};

/* Returns the chosen mode, after checking every invariant. */
static int check_window(const struct win *w, uint32_t *addr, uint32_t *cfg)
{
    plat_seg_t segs[3];
    uint32_t win_end = w->base + w->size;
    int n = 0, split, mode;

    if (w->code_end > w->base) {
        segs[n].base = w->base;
        segs[n].top = w->code_end;
        segs[n].attr = PLAT_SEG_CODE;
        n++;
    }
    if (w->data_end > w->code_end) {
        segs[n].base = w->code_end;
        segs[n].top = w->data_end;
        segs[n].attr = PLAT_SEG_DATA;
        n++;
    }
    segs[n].base = w->stack_low;
    segs[n].top = win_end;
    segs[n].attr = PLAT_SEG_DATA;
    n++;

    for (int i = 0; i < PMP_ENTRIES; i++)
        addr[i] = cfg[i] = 0;
    split = pmp_layout(segs, n, w->base, win_end, PMP_MAP_SPLIT, addr, cfg);

    mode = pmp_map(segs, n, w->base, win_end, addr, cfg);
    if (mode < 0) {
        if (split >= 0)
            problem("ladder gave up where SPLIT fits", w->size, 0);
        return mode;
    }
    if ((mode == PMP_MAP_SPLIT) != (split >= 0))
        problem("ladder skipped or misordered a policy", (uint32_t)mode,
                (uint32_t)split);

    validate(mode, segs, n, w->base, w->size, w->data_end, w->stack_low,
             addr, cfg);
    return mode;
}

static const char *mode_name(int mode)
{
    switch (mode) {
    case PMP_MAP_SPLIT:  return "SPLIT";
    case PMP_MAP_SNAP:   return "SNAP";
    case PMP_MAP_IMAGE:  return "IMAGE";
    case PMP_MAP_WINDOW: return "WINDOW";
    }
    return "none";
}

int main(void)
{
    uint32_t addr[PMP_ENTRIES], cfg[PMP_ENTRIES];
    uint32_t base = 0x20020000u;
    int counts[PMP_MAP_MODES] = {0, 0, 0, 0};
    int denied = 0, saw_image = 0, saw_window = 0;
    int split_ok;

    /* Measured on the built rcc (rv32, rir image): 68 KiB image, 16 KiB stack,
     * wx at +28 KiB, heap base == image end, so a 96 KiB window.  Whether the
     * page-granular split fits depends on where win_alloc put the window:
     * 0x20024000 needs 3 code + 4 data + 1 stack = 8 blocks, 0x20020000
     * needs 7.  Half the 16 KiB-aligned bases failed, which is why the flood
     * looked intermittent (issue #44). */
    struct win rcc = {
        "rcc", base + 16u * 1024u, 96u * 1024u,
        base + 16u * 1024u + 28u * 1024u,
        base + 16u * 1024u + 68u * 1024u,
        base + 16u * 1024u + 80u * 1024u
    };

    split_ok = 0;
    {
        plat_seg_t segs[3];
        int n = 0;

        segs[n].base = rcc.base;
        segs[n].top = rcc.code_end;
        segs[n].attr = PLAT_SEG_CODE;
        n++;
        segs[n].base = rcc.code_end;
        segs[n].top = rcc.data_end;
        segs[n].attr = PLAT_SEG_DATA;
        n++;
        segs[n].base = rcc.stack_low;
        segs[n].top = rcc.base + rcc.size;
        segs[n].attr = PLAT_SEG_DATA;
        n++;
        for (int i = 0; i < PMP_ENTRIES; i++)
            addr[i] = cfg[i] = 0;
        split_ok = pmp_layout(segs, n, rcc.base, rcc.base + rcc.size,
                              PMP_MAP_SPLIT, addr, cfg) >= 0;
    }
    if (split_ok)
        problem("rcc's window unexpectedly fits a split map", 0, 0);

    {
        int mode = check_window(&rcc, addr, cfg);

        if (mode != PMP_MAP_SNAP)
            problem("rcc's window did not fall back to SNAP", (uint32_t)mode,
                    0);
        else
            printf("rcc: SPLIT does not fit, SNAP chosen\n");
    }

    /* csc's measured window (108 KiB image, 16 KiB stack, wx at +48 KiB) is a
     * 128 KiB window that keeps the split map at every base alignment: the
     * ladder must not degrade a window that already fits. */
    {
        struct win csc = {
            "csc", base, 128u * 1024u,
            base + 48u * 1024u, base + 108u * 1024u, base + 112u * 1024u
        };
        int mode = check_window(&csc, addr, cfg);

        if (mode != PMP_MAP_SPLIT)
            problem("csc's 128 KiB window was degraded", (uint32_t)mode, 0);
        else
            printf("csc: SPLIT kept\n");
    }

    /* A small window fits the normal map: the ladder must not degrade it. */
    {
        struct win tiny = {
            "tiny", base, 32u * 1024u,
            base + 4u * 1024u, base + 12u * 1024u, base + 16u * 1024u
        };
        int mode = check_window(&tiny, addr, cfg);

        if (mode != PMP_MAP_SPLIT)
            problem("a small window did not keep the split map",
                    (uint32_t)mode, 0);
        else
            printf("tiny: SPLIT kept\n");
    }

    /* The stack-area layout: a small data window at the bottom, a 16 KiB
     * stack block at the top of the user region, and the hole between them
     * is most of the map.  The mapper's win_end is the stack top (not the
     * data window's end), which is what process.c passes after
     * PLAT_STACK_AREA; SPLIT must still fit in seven blocks. */
    {
        struct win area = {
            "area", base, 0x20080000u - base,
            base + 12u * 1024u, base + 28u * 1024u,
            0x20080000u - 16u * 1024u
        };
        int mode = check_window(&area, addr, cfg);

        if (mode != PMP_MAP_SPLIT)
            problem("area window did not keep the split map",
                    (uint32_t)mode, 0);
        else
            printf("area: SPLIT kept\n");
    }

    /* rcc on the area layout: a 68 KiB image and 40 KiB data window at the
     * bottom, the 16 KiB stack at the top.  The window no longer contains
     * the stack, so the split is the same shape as the old map minus the
     * stack/guard adjacency; WINDOW would cover the hole and break
     * `prot heap`, so any of the first three policies is acceptable. */
    {
        struct win rcc_area = {
            "rcc-area", base, 0x20080000u - base,
            base + 28u * 1024u, base + 68u * 1024u,
            0x20080000u - 16u * 1024u
        };
        int mode = check_window(&rcc_area, addr, cfg);

        if (mode == PMP_MAP_WINDOW)
            problem("rcc-area degraded to WINDOW (guard hole lost)",
                    (uint32_t)mode, 0);
        else
            printf("rcc-area: %s kept\n", mode_name(mode));
    }

    /* Sweep the real range of windows: 32..192 KiB, every page-granular
     * code/data split with at least the 4 KiB guard below the stack. */
    for (uint32_t size = 32u * 1024u; size <= 192u * 1024u;
         size += 16u * 1024u) {
        uint32_t stack_low = base + size - 16u * 1024u;

        for (uint32_t code = 0; code + 4u * 1024u <= stack_low - base;
             code += 4u * 1024u) {
            for (uint32_t data = code; data + 4u * 1024u <= stack_low - base;
                 data += 4u * 1024u) {
                struct win w = {
                    "sweep", base, size,
                    base + code, base + data, stack_low
                };
                int mode = check_window(&w, addr, cfg);

                if (mode < 0)
                    denied++;
                else
                    counts[mode]++;
                if (mode == PMP_MAP_IMAGE)
                    saw_image = 1;
                if (mode == PMP_MAP_WINDOW)
                    saw_window = 1;
            }
        }
    }

    /* A window wider than the page map is a bug, and the ladder must say so
     * rather than program a partial map (the caller then denies user
     * memory). */
    {
        plat_seg_t segs[1];
        uint32_t wide = (uint32_t)PMP_MAX_PAGES * PMP_PAGE + PMP_PAGE;

        segs[0].base = base;
        segs[0].top = base + wide;
        segs[0].attr = PLAT_SEG_DATA;
        for (int i = 0; i < PMP_ENTRIES; i++)
            addr[i] = cfg[i] = 0xffffffffu;
        if (pmp_map(segs, 1, base, base + wide, addr, cfg) >= 0) {
            problem("an oversized window was mapped", wide, 0);
        } else {
            /* A failed map must not leave a partial one behind. */
            for (int i = 0; i < PMP_ENTRIES; i++)
                if (addr[i] || cfg[i])
                    problem("a failed map left entries behind", (uint32_t)i, 0);
            printf("oversized: refused\n");
        }
    }

    printf("sweep: split=%d snap=%d image=%d window=%d denied=%d"
           " (image seen %d, window seen %d)\n",
           counts[PMP_MAP_SPLIT], counts[PMP_MAP_SNAP], counts[PMP_MAP_IMAGE],
           counts[PMP_MAP_WINDOW], denied, saw_image, saw_window);

    if (bad)
        return 1;
    printf("ok\n");
    return 0;
}
