/* NAPOT window mapping for the RP2350 Hazard3 PMP.
 *
 * This is in a header, not in platform.c, so the mapping algorithm can be
 * built and exercised on the host (internal/build/testdata/pmp_map_harness.c
 * via pmpmap_test.go).  It has no MMIO and no assembly: the caller owns the
 * CSR writes, and the includer supplies plat_seg_t, PLAT_SEG_CODE and
 * PLAT_APP_GRAIN.
 *
 * Hazard3 supports NAPOT and NA4 but not TOR: pmpcfg A=1 is WARL-mapped to
 * OFF, so a TOR region never matches.  A window is therefore described by
 * NAPOT blocks: the code run, the data+heap run and the stack run are each
 * covered by as many naturally aligned power-of-two blocks as they need
 * (largest first), and the guard hole between the heap and the stack is
 * deliberately left uncovered so a heap overrun or stack underrun faults.
 *
 * The budget, not alignment, is the limit: the part has eight software-writable
 * regions (0..7), one of which (PMP_VEC_ENTRY) is the trap vector, so seven
 * blocks are available.  8..10 are hardwired by the bootrom (ROM and
 * peripherals, RWX) and 11..15 hardwired OFF; writing those is a no-op.
 *
 * A window can need more blocks than that, because the boundaries are page
 * granular and an unlucky `wx` splits the image into two runs whose aligned
 * decompositions both end in small blocks.  Measured case (issue #44): rcc's
 * 96 KiB window with wx at +28 KiB needs 3 code + 4 data + 1 stack = 8
 * blocks, which is one too many; whether it does depends on which 16 KiB
 * boundary win_alloc handed out, so half the boots failed.  pmp_map()
 * therefore walks a ladder of policies, giving up the least protection that
 * makes the map fit, and reports which one it used so the port can say so
 * once. */
#ifndef TOS_BOARD_RP2350RV_PMP_MAP_H
#define TOS_BOARD_RP2350RV_PMP_MAP_H

#include <stdint.h>

#define PMP_NAPOT 0x18u
#define PMP_R     0x01u
#define PMP_W     0x02u
#define PMP_X     0x04u

#define PMP_ENTRIES   8
#define PMP_VEC_ENTRY 5
#define PMP_PAGE  0x1000u

#define PMP_PAGE_CODE  0x1u
#define PMP_PAGE_DATA  0x2u
/* One byte per 4 KiB page; the whole task area is 384 KiB = 96 pages, so a
 * fixed buffer keeps this off the heap - it runs on every task switch. */
#define PMP_MAX_PAGES  128

/* Mapping policies, in order of decreasing protection:
 *
 *   SPLIT   page-granular RX prefix + RWX data + stack: the normal map.
 *   SNAP    the RX/RWX boundary rounded down to a grain, so both runs are
 *           grain aligned and cost fewer blocks (at most one grain of .text
 *           becomes writable, which is what the wx page already does).
 *   IMAGE   the whole image range RWX + stack: code and rodata lose their RX.
 *   WINDOW  one RWX range over the entire window, guard hole included: last
 *           resort, `prot heap`/`prot stack` stop faulting.
 *
 * Only the first three keep the guard hole uncovered. */
#define PMP_MAP_SPLIT   0
#define PMP_MAP_SNAP    1
#define PMP_MAP_IMAGE   2
#define PMP_MAP_WINDOW  3
#define PMP_MAP_MODES   4

static inline uint32_t napot(uint32_t base, uint32_t size)
{
    return (base >> 2) | ((size >> 3) - 1u);
}

/* Cover [base, top) with NAPOT blocks, recording entry numbers in addr[] and
 * cfg[].  Returns the next free entry, or -1 when the range cannot be named.
 *
 * Every block is at least one page and naturally aligned, and a block never
 * spills past top: overshooting would cover the guard hole below the stack and
 * a stack overflow would stop faulting.  Page granularity is a budget decision,
 * not a hardware limit: RP2350's Hazard3 runs with PMP_GRAIN = 3 (the Hazard3
 * manual: minimum region = 1 << (G + 2) = 32 bytes; NA4 writes are forced to
 * OFF), so 32-byte blocks are legal but a 12 KiB run would want hundreds of
 * them and only 7 entries are free.  See the note in docs/OPEN-ITEMS.md about
 * instructions that cross a block boundary. */
static inline int pmp_cover(uint32_t base, uint32_t top, uint32_t perm,
                            uint32_t *addr, uint32_t *cfg, int next)
{
    while (base < top) {
        uint32_t rem = top - base;
        uint32_t size = 1u << 20;

        if (next == PMP_VEC_ENTRY)
            next++;
        if (next >= PMP_ENTRIES)
            return -1;
        while (size > PMP_PAGE &&
               (size > rem || (base & (size - 1u)) != 0))
            size >>= 1;
        if (size > rem || (base & (size - 1u)) != 0)
            return -1;                 /* tail not representable */
        addr[next] = napot(base, size);
        cfg[next] = perm | PMP_NAPOT;
        next++;
        base += size;
    }
    return next;
}

/* Attributes per page, derived from the segment list.
 *
 * A page that holds code only stays R+X; any page that also touches data or
 * the stack becomes R+W+X, because Hazard3 cannot express a permission union
 * and the page cannot be cut - and that RWX page is the known limitation
 * (OPEN-ITEMS 31.0): wx usually lands inside the same page that holds the
 * start of .data.  The guard hole is not a segment, so its pages stay absent
 * and uncovered, which is what keeps `prot heap` and `prot stack` faulting. */
static inline int pmp_page_attrs(const plat_seg_t *segs, int n,
                                 uint32_t win_base, int pages, uint8_t *attr)
{
    for (int i = 0; i < pages; i++)
        attr[i] = 0;

    for (int i = 0; i < n; i++) {
        uint32_t b = segs[i].base;
        uint32_t t = segs[i].top;
        uint8_t bit = (segs[i].attr == PLAT_SEG_CODE) ? PMP_PAGE_CODE
                                                      : PMP_PAGE_DATA;

        for (uint32_t p = b & ~(PMP_PAGE - 1u); p < t; p += PMP_PAGE) {
            int idx = (int)((p - win_base) / PMP_PAGE);

            if (idx >= 0 && idx < pages)
                attr[idx] |= bit;
        }
    }

    return pages;
}

/* Cover [win_base, win_end) from the segment list under one policy.  Runs of
 * pages with the same attribute become runs of NAPOT blocks; pages that are
 * in no segment are skipped, which is how the hole survives. */
static inline int pmp_layout(const plat_seg_t *segs, int n, uint32_t win_base,
                             uint32_t win_end, int mode, uint32_t *addr,
                             uint32_t *cfg)
{
    uint8_t page[PMP_MAX_PAGES];
    int pages = (int)((win_end - win_base + PMP_PAGE - 1u) / PMP_PAGE);
    int next = 0;

    if (pages > PMP_MAX_PAGES)
        return -1;                      /* a window this wide is a bug */

    pmp_page_attrs(segs, n, win_base, pages, page);

    if (mode == PMP_MAP_WINDOW) {
        /* Not even the hole is left out: one RWX range over the window. */
        for (int i = 0; i < pages; i++)
            page[i] = PMP_PAGE_DATA;
    } else if (mode == PMP_MAP_IMAGE) {
        /* One RWX range for the image, the stack still separate. */
        for (int i = 0; i < pages; i++)
            if (page[i])
                page[i] = PMP_PAGE_DATA;
    } else if (mode == PMP_MAP_SNAP) {
        /* Round the code/data split down to a grain boundary.  Pages before
         * it were code-only (the split starts at the first page that also
         * touches data), so they stay RX; the rest of the image is RWX.  A
         * hole page is still a hole. */
        int step = (int)(PLAT_APP_GRAIN / PMP_PAGE);
        int split = pages;

        if (step < 1)
            step = 1;
        for (int i = 0; i < pages; i++)
            if (page[i] & PMP_PAGE_DATA) {
                split = i & ~(step - 1);
                break;
            }
        for (int i = split; i < pages; i++)
            if (page[i])
                page[i] = PMP_PAGE_DATA;
    }

    for (int i = 0; i < pages; ) {
        uint32_t perm, top;
        int j;

        if (!page[i]) {                 /* hole: never covered */
            i++;
            continue;
        }
        perm = (page[i] & PMP_PAGE_DATA) ? (PMP_R | PMP_W | PMP_X)
                                         : (PMP_R | PMP_X);
        for (j = i; j < pages && page[j] == page[i]; j++)
            ;
        /* A block may not spill past the window: a page that is only partly
         * inside it stops the run.  On this board stack_top is page aligned,
         * so this is a guard against a future layout, not a live case. */
        top = win_base + (uint32_t)j * PMP_PAGE;
        if (top > win_end)
            top = win_end;
        next = pmp_cover(win_base + (uint32_t)i * PMP_PAGE, top, perm,
                         addr, cfg, next);
        if (next < 0)
            return -1;
        i = j;
    }
    return next;
}

/* The ladder: try the policies in order and fill addr/cfg for the first that
 * fits.  Returns the policy used, or -1 when even PMP_MAP_WINDOW does not fit.
 * addr/cfg belong to the caller (PMP_ENTRIES each) and are zeroed. */
static inline int pmp_map(const plat_seg_t *segs, int n, uint32_t win_base,
                          uint32_t win_end, uint32_t *addr, uint32_t *cfg)
{
    int next = -1, mode = -1;

    for (int m = 0; m < PMP_MAP_MODES && next < 0; m++) {
        for (int i = 0; i < PMP_ENTRIES; i++) {
            addr[i] = 0;
            cfg[i] = 0;
        }
        next = pmp_layout(segs, n, win_base, win_end, m, addr, cfg);
        if (next >= 0)
            mode = m;
    }
    if (next < 0) {
        /* The last attempt failed part way through; do not leave its partial
         * blocks for the caller to program. */
        for (int i = 0; i < PMP_ENTRIES; i++)
            addr[i] = cfg[i] = 0;
        return -1;
    }
    return mode;
}

#endif /* TOS_BOARD_RP2350RV_PMP_MAP_H */
