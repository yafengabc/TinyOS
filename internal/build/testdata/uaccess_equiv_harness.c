/* Characterisation harness for the #54 pure move.
 *
 * The old implementations are copied here verbatim from elf.c:127 and
 * process.c:445 (pre-extraction).  The Go test compiles this file against the
 * REAL kernel/uaccess.c and kernel/u32math.h and checks that both versions
 * agree on boundary and pseudo-random inputs.  If someone later "fixes" a
 * branch while moving code, this fails with the exact tuple that differs.
 *
 * This is a before/after equivalence check, not a spec of the right answer;
 * #53 owns the rule-level host tests (len==0, wrap, grain alignment, ...). */
/* kernel/types.h defines uintptr_t/intptr_t differently from the host CRT;
 * including <stdio.h> pulls corecrt.h in and redefines them.  Declare printf
 * directly so we can still include the real kernel headers. */
int printf(const char *fmt, ...);

#include "uaccess.h"
#include "u32math.h"

/* --- pre-extraction copies, do not "improve" --- */
struct old_view { uint32_t lo, hi; };

static int old_user_range_ok(const struct old_view *v, uint32_t va, uint32_t len)
{
    if (len == 0)
        return 1;
    if (va < v->lo)
        return 0;
#ifndef BREAK_OVERFLOW_BRANCH
    if (va + len < va) /* overflow */
        return 0;
#endif
    return va + len <= v->hi;
}

static uint32_t old_align_up(uint32_t v, uint32_t a)
{
    return (v + a - 1) & ~(a - 1);
}

static int old_add_u32_ok(uint32_t a, uint32_t b, uint32_t *out)
{
    if (a > 0xFFFFFFFFu - b)
        return -1;
    *out = a + b;
    return 0;
}

static int old_align_up_ok(uint32_t v, uint32_t a, uint32_t *out)
{
    uint32_t rounded;

    if (a == 0 || (a & (a - 1u)) != 0)
        return -1;
    if (v > 0xFFFFFFFFu - (a - 1u))
        return -1;
    rounded = (v + a - 1u) & ~(a - 1u);
    if (rounded < v)
        return -1;
    *out = rounded;
    return 0;
}

static int failures;

static void check_range(uint32_t lo, uint32_t hi, uint32_t va, uint32_t len)
{
    struct old_view v = { lo, hi };
    int old_r = old_user_range_ok(&v, va, len);
    int new_r = uaccess_range_ok(lo, hi, va, len);

    if (old_r != new_r) {
        printf("FAIL range lo=%08x hi=%08x va=%08x len=%08x: old=%d new=%d\n",
               lo, hi, va, len, old_r, new_r);
        failures++;
    }
}

static void check_add(uint32_t a, uint32_t b)
{
    uint32_t x = 0, y = 0;
    int ox = old_add_u32_ok(a, b, &x);
    int nx = add_u32_ok(a, b, &y);
    if (ox != nx || x != y) {
        printf("FAIL add %08x+%08x: old=%d/%08x new=%d/%08x\n", a, b, ox, x, nx, y);
        failures++;
    }
}

static void check_align_ok(uint32_t v, uint32_t a)
{
    uint32_t x = 0, y = 0;
    int ox = old_align_up_ok(v, a, &x);
    int nx = align_up_ok(v, a, &y);
    if (ox != nx || x != y) {
        printf("FAIL align_ok v=%08x a=%08x: old=%d/%08x new=%d/%08x\n",
               v, a, ox, x, nx, y);
        failures++;
    }
    if (a != 0 && ox == 0 && old_align_up(v, a) != x) {
        printf("FAIL align plain v=%08x a=%08x: %08x vs checked %08x\n",
               v, a, old_align_up(v, a), x);
        failures++;
    }
}

/* xorshift32: reproducible without pulling libc rand() into the comparison. */
static uint32_t next_rand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

int main(void)
{
    static const uint32_t interesting[] = {
        0, 1, 2, 3, 4, 7, 8, 15, 16,
        0x00000FFFu, 0x00001000u, 0x00001001u,
        0x7FFFFFFFu, 0x80000000u,
        0xFFFFFFFEu, 0xFFFFFFFFu,
    };
    const int n = (int)(sizeof(interesting) / sizeof(interesting[0]));
    uint32_t seed = 0x12345678u;
    int i, j, k;

    /* Exhaustive over interesting values for all four inputs' roles. */
    for (i = 0; i < n; i++) {
        check_add(interesting[i], 1);
        check_add(0xFFFFFFFFu, interesting[i]);
        check_align_ok(interesting[i], interesting[i] | 1u);
        for (j = 0; j < n; j++) {
            check_add(interesting[i], interesting[j]);
            check_align_ok(interesting[i], interesting[j]);
            for (k = 0; k < n; k++) {
                check_range(interesting[i], interesting[j],
                            interesting[k], interesting[(i + j + k) % n]);
            }
        }
    }

    /* 20k pseudo-random tuples on top of the boundary grid. */
    for (i = 0; i < 20000; i++) {
        uint32_t a = next_rand(&seed);
        uint32_t b = next_rand(&seed);
        uint32_t c = next_rand(&seed);
        uint32_t d = next_rand(&seed);
        check_range(a, b, c, d);
        check_add(a, b);
        check_align_ok(a, b);
    }

    if (failures) {
        printf("uaccess equivalence: %d mismatches\n", failures);
        return 1;
    }
    printf("uaccess equivalence: ok (%d boundary + 20000 random tuples)\n", n * n * n);
    return 0;
}
