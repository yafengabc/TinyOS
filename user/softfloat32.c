#include "softfloat32.h"
#include "lib.h"

/* Native binary32 soft float.
 *
 * Bit layout of the working significand (27 bits, fits a uint32_t):
 *
 *     bit 26      leading one (value normalised to [1,2))
 *     bits 25..3  the 23 fraction bits
 *     bit  2      guard   (round if set)
 *     bit  1      round   (with the guard, decides a tie)
 *     bit  0      sticky  (something was lost below)
 *
 * Every operation returns a value in this form to sf_finish(), which shifts it
 * to the leading-one position, rounds once (nearest, ties to even) and packs.
 * One rounding site instead of one per operation is what keeps 1.0 + 1.0 from
 * rounding to 1.0 and x/y from drifting a ulp per operation.
 *
 * With V in this form, a value with biased exponent e is V * 2^(e-153):
 * V = M << 3 where M is the 24-bit significand and value = M * 2^(e-150). */

typedef union {
    float f;
    uint32_t u;
} f32bits;

#define F_SIGN 0x80000000u
#define F_EXP  0x7F800000u
#define F_FRAC 0x007FFFFFu
#define F_QNAN 0x7FC00000u
#define F_INF  0x7F800000u

#define SF_LEAD  0x04000000u    /* bit 26 */
#define SF_ABOVE 0xF8000000u    /* bits 27..31: carries/oversized products */

/* Keep the bits opaque to the optimiser.  LLVM recognises the integer tests
 * below as isnan/iszero and folds them back into fcmp, which on a target with
 * no FPU becomes a call to the very helper being written (__eqsf2 recursing
 * into __eqsf2).  An empty asm is enough to break the value tracking; it
 * costs no instructions.  goc's double runtime documents the same trap. */
static uint32_t sf_launder(uint32_t u)
{
    __asm__ volatile("" : "+r"(u));
    return u;
}

static float from_bits(uint32_t u)
{
    f32bits x;

    x.u = sf_launder(u);
    return x.f;
}

static uint32_t to_bits(float f)
{
    f32bits x;

    x.f = f;
    return sf_launder(x.u);
}

static int sf_is_nan(uint32_t u)
{
    return (u & F_EXP) == F_EXP && (u & F_FRAC) != 0;
}

static int sf_is_inf(uint32_t u)
{
    return (u & 0x7FFFFFFFu) == F_INF;
}

static int sf_is_zero(uint32_t u)
{
    return (u & 0x7FFFFFFFu) == 0;
}

static uint32_t sf_zero(int neg)
{
    return neg ? F_SIGN : 0u;
}

static uint32_t sf_inf(int neg)
{
    return (neg ? F_SIGN : 0u) | F_INF;
}

/* Right shift that folds every lost bit into the sticky position. */
static uint32_t sf_shr_sticky(uint32_t v, int n)
{
    uint32_t lost;

    if (n <= 0)
        return v;
    if (n >= 32)
        return v ? 1u : 0u;
    lost = v & ((1u << n) - 1u);
    v >>= n;
    if (lost)
        v |= 1u;
    return v;
}

/* Return the 27-bit working significand and the biased exponent.  Subnormals
 * come out normalised with a biased exponent below 1. */
static uint32_t sf_unpack(uint32_t u, int *ep)
{
    int e = (int)((u >> 23) & 0xFF);
    uint32_t m = u & F_FRAC;
#ifndef TOS_SOFTFLOAT_MIN
    int sh;
#endif

    if (e == 0) {
#ifdef TOS_SOFTFLOAT_MIN
        /* No subnormals: everything below 2^-126 is zero. */
        *ep = 0;
        return 0;
#else
        *ep = 0;
        if (m == 0)
            return 0;
        sh = 0;
        while (!(m & 0x00800000u)) {
            m <<= 1;
            sh++;
        }
        *ep = 1 - sh;
        return m << 3;
#endif
    }
#ifdef TOS_SOFTFLOAT_MIN
    if (e == 0xFF) {            /* no NaN/Inf: read as the largest finite */
        *ep = 0xFE;
        return 0x7FFFFF8u;
    }
#endif
    *ep = e;
    return (m | 0x00800000u) << 3;
}

/* Round and pack V (27-ish bits, value = V * 2^(e-153)). */
static float sf_finish(uint32_t v, int e, int neg)
{
    uint32_t guard, round, sticky, frac;

    if (v == 0)
        return from_bits(sf_zero(neg));

    while (v & SF_ABOVE) {
        uint32_t lsb = v & 1u;

        v >>= 1;
        e++;
        if (lsb)
            v |= 1u;            /* fold the shifted-out bit into sticky */
    }
    while (!(v & SF_LEAD)) {
        v <<= 1;
        e--;
    }

    guard = v & 4u;
    round = v & 2u;
    sticky = v & 1u;
    frac = v >> 3;              /* 24-bit significand, leading one at bit 23 */

#ifdef TOS_SOFTFLOAT_MIN
    /* Saturation instead of Inf; underflow is zero. */
    if (e >= 0xFF)
        return from_bits((neg ? F_SIGN : 0u) | 0x7F7FFFFFu);
    if (e <= 0)
        return from_bits(sf_zero(neg));
#else
    if (e >= 0xFF)
        return from_bits(sf_inf(neg));
    if (e <= 0) {
        /* Subnormal: round once, at the subnormal scale.  Using v (which still
         * carries the guard/round/sticky bits) keeps this a single rounding;
         * rounding the 24-bit significand first and shifting again would
         * round twice and land a ulp off on some values. */
        int k = 1 - e + 3;      /* target LSB, in v's 2^-3-ulp units */
        uint32_t w, rem, half;

        if (k >= 32)
            return from_bits(sf_zero(neg));
        w = v >> k;
        rem = v & ((1u << k) - 1u);
        half = 1u << (k - 1);
        if (rem > half || (rem == half && (w & 1u)))
            w++;
        /* w == 0x800000 carries into exponent 1, the smallest normal. */
        return from_bits((neg ? F_SIGN : 0u) | w);
    }
#endif
    if (guard && (round || sticky || (frac & 1u))) {
        frac++;
        if (frac == 0x01000000u) {
            frac >>= 1;
            e++;
        }
    }
#ifdef TOS_SOFTFLOAT_MIN
    if (e >= 0xFF)
        return from_bits((neg ? F_SIGN : 0u) | 0x7F7FFFFFu);
#else
    if (e >= 0xFF)
        return from_bits(sf_inf(neg));
#endif
    return from_bits((neg ? F_SIGN : 0u) | ((uint32_t)e << 23) |
                     (frac & F_FRAC));
}

/* ---- addition and subtraction ---------------------------------------- */

static float sf_addsub(float fa, float fb, int sub)
{
    uint32_t ua = to_bits(fa), ub = to_bits(fb);
    uint32_t va, vb, sum;
    int ea, eb, e, na, nb, neg;

#ifndef TOS_SOFTFLOAT_MIN
    if (sf_is_nan(ua) || sf_is_nan(ub))
        return from_bits(F_QNAN);
#endif
    if (sub)
        ub ^= F_SIGN;
    na = (int)(ua >> 31);
    nb = (int)(ub >> 31);

#ifndef TOS_SOFTFLOAT_MIN
    if (sf_is_inf(ua) || sf_is_inf(ub)) {
        if (sf_is_inf(ua) && sf_is_inf(ub)) {
            if (na != nb)
                return from_bits(F_QNAN);       /* inf - inf */
            return from_bits(sf_inf(na));
        }
        neg = sf_is_inf(ua) ? na : nb;
        return from_bits(sf_inf(neg));
    }
#endif
    if (sf_is_zero(ua) && sf_is_zero(ub))
        return from_bits(sf_zero(sub && na == nb));
    if (sf_is_zero(ua))
        return from_bits(ub);
    if (sf_is_zero(ub))
        return from_bits(ua);

    va = sf_unpack(ua, &ea);
    vb = sf_unpack(ub, &eb);
    if (ea > eb) {
        e = ea;
        vb = sf_shr_sticky(vb, ea - eb);
    } else if (eb > ea) {
        e = eb;
        va = sf_shr_sticky(va, eb - ea);
    } else {
        e = ea;
    }

    if (na != nb) {
        if (va == vb)
            return from_bits(0);                /* x + (-x) */
        if (va > vb) {
            sum = va - vb;
            neg = na;
        } else {
            sum = vb - va;
            neg = nb;
        }
    } else {
        sum = va + vb;
        neg = na;
    }
    return sf_finish(sum, e, neg);
}

float __addsf3(float a, float b) { return sf_addsub(a, b, 0); }
float __subsf3(float a, float b) { return sf_addsub(a, b, 1); }

float __negsf2(float a)
{
    return from_bits(to_bits(a) ^ F_SIGN);
}

/* ---- multiplication --------------------------------------------------- */

float __mulsf3(float a, float b)
{
    uint32_t ua = to_bits(a), ub = to_bits(b);
    uint32_t ma, mb, hi, lo, v, lost;
    int ea, eb, e, s, neg, top;

#ifndef TOS_SOFTFLOAT_MIN
    if (sf_is_nan(ua) || sf_is_nan(ub))
        return from_bits(F_QNAN);
#endif
    neg = (int)((ua ^ ub) >> 31);
#ifndef TOS_SOFTFLOAT_MIN
    if (sf_is_inf(ua) || sf_is_inf(ub)) {
        if (sf_is_zero(ua) || sf_is_zero(ub))
            return from_bits(F_QNAN);           /* inf * 0 */
        return from_bits(sf_inf(neg));
    }
#endif
    if (sf_is_zero(ua) || sf_is_zero(ub))
        return from_bits(sf_zero(neg));

    ma = sf_unpack(ua, &ea) >> 3;               /* 24-bit significands */
    mb = sf_unpack(ub, &eb) >> 3;
    /* 24x24 -> 48 bit product from 16-bit halves.  A uint64_t multiply would
     * become __aeabi_lmul on the Cortex-M0+, which has no UMULL. */
    {
        uint32_t a0 = ma & 0xFFFFu, a1 = ma >> 16;
        uint32_t b0 = mb & 0xFFFFu, b1 = mb >> 16;
        uint32_t p0 = a0 * b0;
        uint32_t p1 = a0 * b1;
        uint32_t p2 = a1 * b0;
        uint32_t p3 = a1 * b1;
        uint32_t mid = (p0 >> 16) + (p1 & 0xFFFFu) + (p2 & 0xFFFFu);

        lo = (mid << 16) | (p0 & 0xFFFFu);
        hi = p3 + (p1 >> 16) + (p2 >> 16) + (mid >> 16);
    }
    top = hi ? 47 : 46;
    s = top - 26;                               /* 20 or 21 */
    v = (lo >> s) | (hi << (32 - s));
    lost = lo & ((1u << s) - 1u);
    if (lost)
        v |= 1u;
    e = ea + eb - 147 + s;
    return sf_finish(v, e, neg);
}

/* ---- division --------------------------------------------------------- */

float __divsf3(float a, float b)
{
    uint32_t ua = to_bits(a), ub = to_bits(b);
    uint32_t ma, mb, q, r, v, tmp;
    int ea, eb, e, t, neg, msb;

#ifndef TOS_SOFTFLOAT_MIN
    if (sf_is_nan(ua) || sf_is_nan(ub))
        return from_bits(F_QNAN);
#endif
    neg = (int)((ua ^ ub) >> 31);
#ifndef TOS_SOFTFLOAT_MIN
    if (sf_is_inf(ua)) {
        if (sf_is_inf(ub))
            return from_bits(F_QNAN);           /* inf / inf */
        return from_bits(sf_inf(neg));
    }
    if (sf_is_inf(ub))
        return from_bits(sf_zero(neg));
    if (sf_is_zero(ub)) {
        if (sf_is_zero(ua))
            return from_bits(F_QNAN);           /* 0 / 0 */
        return from_bits(sf_inf(neg));
    }
#else
    if (sf_is_zero(ub)) {
        if (sf_is_zero(ua))
            return from_bits(sf_zero(neg));
        return from_bits((neg ? F_SIGN : 0u) | 0x7F7FFFFFu);
    }
#endif
    if (sf_is_zero(ua))
        return from_bits(sf_zero(neg));

    ma = sf_unpack(ua, &ea) >> 3;
    mb = sf_unpack(ub, &eb) >> 3;

    /* q = floor(ma * 2^30 / mb), exactly.  ma/mb is in (0.5, 2), so the
     * integer part is one bit and q stays below 2^31.  Handling that bit
     * separately keeps the restoring loop's precondition (r < mb) intact
     * without throwing away a significand bit. */
    q = 0;
    r = ma;
    if (r >= mb) {
        r -= mb;
        q = 1u << 30;
    }
    for (int i = 29; i >= 0; i--) {
        r <<= 1;
        if (r >= mb) {
            r -= mb;
            q |= 1u << i;
        }
    }
    msb = 0;
    tmp = q;
    while (tmp >>= 1)
        msb++;
    t = msb - 26;                               /* 3 or 4 */
    v = q >> t;
    if ((q & ((1u << t) - 1u)) || r)
        v |= 1u;
    e = ea - eb + 123 + t;
    return sf_finish(v, e, neg);
}

/* ---- comparisons ------------------------------------------------------ */

/* -1, 0 or 1; NaN is reported as 0, every caller checks it first. */
static int sf_cmp(float a, float b)
{
    uint32_t ua = to_bits(a), ub = to_bits(b);
    int sa, sb, c;

#ifndef TOS_SOFTFLOAT_MIN
    if (sf_is_nan(ua) || sf_is_nan(ub))
        return 0;
#endif
    if ((ua << 1) == 0 && (ub << 1) == 0)
        return 0;                               /* -0 == +0 */
    sa = (int)(ua >> 31);
    sb = (int)(ub >> 31);
    if (sa != sb)
        return sa ? -1 : 1;
    if (ua == ub)
        return 0;
    c = ua < ub ? -1 : 1;                       /* same sign: bits order it */
    return sa ? -c : c;
}

int __eqsf2(float a, float b)
{
#ifndef TOS_SOFTFLOAT_MIN
    if (sf_is_nan(to_bits(a)) || sf_is_nan(to_bits(b)))
        return 1;
#endif
    return sf_cmp(a, b) == 0 ? 0 : 1;
}

int __nesf2(float a, float b)
{
#ifndef TOS_SOFTFLOAT_MIN
    if (sf_is_nan(to_bits(a)) || sf_is_nan(to_bits(b)))
        return 0;
#endif
    return sf_cmp(a, b) == 0 ? 0 : 1;
}

int __ltsf2(float a, float b) { return sf_cmp(a, b); }
int __gtsf2(float a, float b) { return sf_cmp(a, b); }

int __lesf2(float a, float b)
{
#ifndef TOS_SOFTFLOAT_MIN
    if (sf_is_nan(to_bits(a)) || sf_is_nan(to_bits(b)))
        return 1;
#endif
    return sf_cmp(a, b) <= 0 ? -1 : 1;
}

int __gesf2(float a, float b)
{
#ifndef TOS_SOFTFLOAT_MIN
    if (sf_is_nan(to_bits(a)) || sf_is_nan(to_bits(b)))
        return -1;
#endif
    return sf_cmp(a, b) >= 0 ? 1 : -1;
}

int __unordsf2(float a, float b)
{
#ifdef TOS_SOFTFLOAT_MIN
    (void)a;
    (void)b;
    return 0;
#else
    return (sf_is_nan(to_bits(a)) || sf_is_nan(to_bits(b))) ? 1 : 0;
#endif
}

/* ---- float <-> integer ------------------------------------------------ */

int __fixsfsi(float a)
{
    uint32_t u = to_bits(a);
    int e = (int)((u >> 23) & 0xFF);
    uint32_t m, mag;
    int sh;

    if (e == 0xFF)
        return (int)(u >> 31) ? (int)0x80000000 : (int)0x7FFFFFFF;
    if (e == 0 || e < 127)
        return 0;                               /* |a| < 1 or subnormal */
    sh = e - 150;
    m = (u & F_FRAC) | 0x00800000u;
    if (sh >= 8)
        return (int)(u >> 31) ? (int)0x80000000 : (int)0x7FFFFFFF;
    mag = sh >= 0 ? (m << sh) : (m >> -sh);
    if (u & F_SIGN) {
        if (mag >= 0x80000000u)
            return (int)0x80000000;
        return -(int)mag;
    }
    if (mag > 0x7FFFFFFFu)
        return (int)0x7FFFFFFF;
    return (int)mag;
}

unsigned int __fixunssfsi(float a)
{
    uint32_t u = to_bits(a);
    int e = (int)((u >> 23) & 0xFF);
    uint32_t m, mag;
    int sh;

    if (e == 0xFF)
        return 0xFFFFFFFFu;
    if (u & F_SIGN)
        return 0;
    if (e == 0 || e < 127)
        return 0;
    sh = e - 150;
    m = (u & F_FRAC) | 0x00800000u;
    if (sh >= 9)
        return 0xFFFFFFFFu;
    mag = sh >= 0 ? (m << sh) : (m >> -sh);
    return mag;
}

static float sf_from_mag(uint32_t m, int neg)
{
    if (m == 0)
        return from_bits(sf_zero(neg));
    /* value = m, and sf_finish reads V * 2^(e-153). */
    return sf_finish(m, 153, neg);
}

float __floatsisf(int a)
{
    if (a < 0) {
        uint32_t m = (a == (int)0x80000000) ? 0x80000000u : (uint32_t)(-a);

        return sf_from_mag(m, 1);
    }
    return sf_from_mag((uint32_t)a, 0);
}

float __floatunsisf(unsigned int a)
{
    return sf_from_mag(a, 0);
}
