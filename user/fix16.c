#include "fix16.h"
#include "lib.h"

fix16_t fix16_from_int(int v)
{
    return (fix16_t)((uint32_t)v << FIX16_SHIFT);
}

int fix16_to_int(fix16_t v)
{
    if (v < 0)
        return -(int)(((uint32_t)0 - (uint32_t)v) >> FIX16_SHIFT);
    return (int)((uint32_t)v >> FIX16_SHIFT);
}

fix16_t fix16_add(fix16_t a, fix16_t b) { return a + b; }
fix16_t fix16_sub(fix16_t a, fix16_t b) { return a - b; }

fix16_t fix16_mul(fix16_t a, fix16_t b)
{
    uint32_t ua, ub, a0, a1, b0, b1, p0, p1, p2, p3, mid, hi, r;
    int neg = (a < 0) != (b < 0);

    /* 32x32 -> 64 from 16-bit halves, so the M0+ (no UMULL) needs no
     * __aeabi_lmul and the whole library stays 32-bit. */
    ua = a < 0 ? (uint32_t)0 - (uint32_t)a : (uint32_t)a;
    ub = b < 0 ? (uint32_t)0 - (uint32_t)b : (uint32_t)b;
    a0 = ua & 0xFFFFu;
    a1 = ua >> 16;
    b0 = ub & 0xFFFFu;
    b1 = ub >> 16;
    p0 = a0 * b0;
    p1 = a0 * b1;
    p2 = a1 * b0;
    p3 = a1 * b1;
    mid = (p0 >> 16) + (p1 & 0xFFFFu) + (p2 & 0xFFFFu);
    hi = p3 + (p1 >> 16) + (p2 >> 16) + (mid >> 16);
    r = (hi << 16) | (mid & 0xFFFFu);
    return (fix16_t)(neg ? (uint32_t)0 - r : r);
}

fix16_t fix16_div(fix16_t a, fix16_t b)
{
    uint32_t ua, ub, q, r, frac, carry;
    int neg, i;

    if (b == 0)
        return a < 0 ? FIX16_MIN : FIX16_MAX;
    neg = (a < 0) != (b < 0);
    ua = a < 0 ? (uint32_t)0 - (uint32_t)a : (uint32_t)a;
    ub = b < 0 ? (uint32_t)0 - (uint32_t)b : (uint32_t)b;

    q = ua / ub;
    r = ua % ub;
    if (q > 0x7FFFu)                    /* integer part alone would overflow */
        return neg ? FIX16_MIN : FIX16_MAX;

    /* Sixteen fractional bits, one at a time, on 32-bit registers.  When the
     * doubling carries out of r the true value is 2r = 2^32 + r', which is
     * still below 2*ub, so the wrapped subtraction below is exact. */
    frac = 0;
    for (i = 0; i < 16; i++) {
        carry = r >> 31;
        r <<= 1;
        if (carry || r >= ub) {
            r -= ub;
            frac |= 1u << (15 - i);
        }
    }
    q = (q << FIX16_SHIFT) | frac;
    if (neg)
        return (fix16_t)((uint32_t)0 - q);
    return (fix16_t)q;
}

fix16_t fix16_abs(fix16_t v)
{
    return v < 0 ? (fix16_t)((uint32_t)0 - (uint32_t)v) : v;
}

/* ---- printing (integer only) ------------------------------------------ */

static int fx_putc(char *out, int cap, int n, char c)
{
    if (n < cap - 1)
        out[n] = c;
    return n + 1;
}

int fix16_to_str(char *out, int cap, fix16_t v, int decimals)
{
    uint32_t mag, ip, frac, scale, part;
    int neg, n = 0, i, digits[4];

    if (cap <= 0)
        return 0;
    neg = v < 0;
    mag = neg ? (uint32_t)0 - (uint32_t)v : (uint32_t)v;
    ip = mag >> FIX16_SHIFT;
    frac = mag & 0xFFFFu;
    if (decimals < 0)
        decimals = 0;
    if (decimals > 4)
        decimals = 4;

    scale = 1;
    for (i = 0; i < decimals; i++)
        scale *= 10;
    part = (frac * scale + 0x8000u) >> FIX16_SHIFT;   /* round half up */
    if (part >= scale) {
        part -= scale;
        ip++;
    }
    if (part > scale - 1)
        part = scale - 1;

    if (neg && (ip || part))
        n = fx_putc(out, cap, n, '-');
    {
        char tmp[10];
        int k = 0;
        uint32_t t = ip;

        if (t == 0)
            tmp[k++] = '0';
        while (t) {
            tmp[k++] = (char)('0' + (t % 10));
            t /= 10;
        }
        while (k--)
            n = fx_putc(out, cap, n, tmp[k]);
    }
    if (decimals > 0) {
        n = fx_putc(out, cap, n, '.');
        for (i = decimals - 1; i >= 0; i--) {
            digits[i] = (int)(part % 10);
            part /= 10;
        }
        for (i = 0; i < decimals; i++)
            n = fx_putc(out, cap, n, (char)('0' + digits[i]));
    }
    out[n < cap - 1 ? n : cap - 1] = '\0';
    return n;
}

int print_fix16_n(fix16_t v, int decimals)
{
    char buf[32];
    int n = fix16_to_str(buf, sizeof(buf), v, decimals);

    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    write_all(1, buf, n);
    return n;
}

int print_fix16(fix16_t v)
{
    return print_fix16_n(v, 4);
}
