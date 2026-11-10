#include "softfloat32.h"
#include "lib.h"

/* Float printing, shared by both runtimes.
 *
 * The conversion is done entirely on the IEEE bit pattern with integer
 * arithmetic, so printing never pulls in the float runtime: a program that
 * only formats a value links ~700 bytes instead of ~1.4 KiB.  printf is left
 * alone; call printfloat()/float_to_str().
 *
 * value = M * 2^(e-150), so value * 10^d = M * 10^d * 2^(e-150).  That
 * product (48 bits) is shifted into a 64-bit D and divided by 10^d once, which
 * gives the integer part and the d decimal digits directly, with a single
 * round-to-nearest on the shift. */

typedef union {
    float f;
    uint32_t u;
} f32bits;

static uint32_t fp_to_bits(float f)
{
    f32bits x;

    x.f = f;
    return x.u;
}

static int fp_putc(char *out, int cap, int n, char c)
{
    if (n < cap - 1)
        out[n] = c;
    return n + 1;
}

static int fp_put_uint(char *out, int cap, int n, uint32_t v)
{
    char tmp[10];
    int k = 0;

    if (v == 0)
        tmp[k++] = '0';
    while (v) {
        tmp[k++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (k--)
        n = fp_putc(out, cap, n, tmp[k]);
    return n;
}

/* 32x32 -> 64 from 16-bit halves (no 64-bit helpers on Cortex-M0+). */
static void fp_mul32(uint32_t a, uint32_t b, uint32_t *hi, uint32_t *lo)
{
    uint32_t a0 = a & 0xFFFFu, a1 = a >> 16;
    uint32_t b0 = b & 0xFFFFu, b1 = b >> 16;
    uint32_t p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0, p3 = a1 * b1;
    uint32_t mid = (p0 >> 16) + (p1 & 0xFFFFu) + (p2 & 0xFFFFu);

    *lo = (mid << 16) | (p0 & 0xFFFFu);
    *hi = p3 + (p1 >> 16) + (p2 >> 16) + (mid >> 16);
}

/* (hi:lo) / d, quotient must fit in 32 bits. */
static uint32_t fp_div64(uint32_t hi, uint32_t lo, uint32_t d, uint32_t *rem)
{
    uint32_t q = 0, r = 0;
    int i;

    for (i = 63; i >= 0; i--) {
        uint32_t bit = (i >= 32) ? ((hi >> (i - 32)) & 1u) : ((lo >> i) & 1u);
        uint32_t carry = r >> 31;

        r = (r << 1) | bit;
        if (carry || r >= d) {
            r -= d;
            if (i < 32)
                q |= 1u << i;
        }
    }
    *rem = r;
    return q;
}

static uint32_t fp_bit(uint32_t hi, uint32_t lo, int i)
{
    return (i >= 32) ? ((hi >> (i - 32)) & 1u) : ((lo >> i) & 1u);
}

/* Any bit set below position n? */
static int fp_below(uint32_t hi, uint32_t lo, int n)
{
    if (n <= 0)
        return 0;
    if (n >= 64)
        return hi != 0 || lo != 0;
    if (n > 32)
        return lo != 0 || (hi & ((1u << (n - 32)) - 1u)) != 0;
    if (n == 32)
        return lo != 0;
    return (lo & ((1u << n) - 1u)) != 0;
}

int float_to_str(char *out, int cap, float v, int decimals)
{
    uint32_t bits = fp_to_bits(v);
    uint32_t e = (bits >> 23) & 0xFF;
    uint32_t m = bits & 0x007FFFFFu;
    int neg = (int)(bits >> 31);
    int n = 0, i, s, k;
    uint32_t pow10 = 1, M, hi, lo, dhi, dlo, ip, frac;
    char digits[8];

    if (cap <= 0)
        return 0;
    if (e == 0xFF) {
        const char *str = m ? "nan" : "inf";

        if (neg && !m)
            n = fp_putc(out, cap, n, '-');
        while (*str)
            n = fp_putc(out, cap, n, *str++);
        out[n < cap - 1 ? n : cap - 1] = '\0';
        return n;
    }
    if (neg)
        n = fp_putc(out, cap, n, '-');
    if (e >= 158) {                             /* |v| >= 2^31 */
        const char *str = "big";

        while (*str)
            n = fp_putc(out, cap, n, *str++);
        out[n < cap - 1 ? n : cap - 1] = '\0';
        return n;
    }
    if (decimals < 0)
        decimals = 0;
    if (decimals > 7)
        decimals = 7;
    for (i = 0; i < decimals; i++)
        pow10 *= 10;

    if (e == 0) {                               /* zero or subnormal */
        M = 0;
        hi = lo = 0;
    } else {
        M = m | 0x00800000u;
        fp_mul32(M, pow10, &hi, &lo);
    }
    s = (int)e - 150;
    if (e == 0)
        dhi = dlo = 0;
    else if (s >= 0) {
        dhi = (hi << s) | (s ? (lo >> (32 - s)) : 0u);
        dlo = lo << s;
    } else {
        k = -s;
        if (k >= 64) {
            dhi = dlo = 0;
        } else {
            uint32_t guard, below;

            if (k >= 32) {
                dhi = 0;
                dlo = hi >> (k - 32);
            } else {
                dhi = hi >> k;
                dlo = (lo >> k) | (hi << (32 - k));
            }
            /* Round to nearest, ties to even, matching printf. */
            guard = fp_bit(hi, lo, k - 1);
            below = fp_below(hi, lo, k - 1);
            if (guard && (below || (dlo & 1u))) {
                dlo++;
                if (dlo == 0)
                    dhi++;
            }
        }
    }

    if (decimals == 0) {
        ip = dlo;
        frac = 0;
    } else {
        ip = fp_div64(dhi, dlo, pow10, &frac);
    }

    n = fp_put_uint(out, cap, n, ip);
    if (decimals > 0) {
        n = fp_putc(out, cap, n, '.');
        for (i = decimals - 1; i >= 0; i--) {
            digits[i] = (char)('0' + (frac % 10));
            frac /= 10;
        }
        for (i = 0; i < decimals; i++)
            n = fp_putc(out, cap, n, digits[i]);
    }
    out[n < cap - 1 ? n : cap - 1] = '\0';
    return n;
}

int printfloat_n(float v, int decimals)
{
    char buf[40];
    int n = float_to_str(buf, sizeof(buf), v, decimals);

    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    write_all(1, buf, n);
    return n;
}

int printfloat(float v)
{
    return printfloat_n(v, 4);
}
