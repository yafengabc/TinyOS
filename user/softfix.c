#include "softfloat32.h"
#include "fix16.h"

/* A `float` runtime backed by Q16.16.
 *
 * Same entry points as softfloat32.c, so source code keeps using ordinary
 * float syntax and the build picks the backend:
 *
 *     float f = 1.0f;
 *     f = f / 2.0f;          // -> __divsf3 -> fix16_div
 *
 * Range is Q16.16's: +/-32767.99998 with a 1/65536 step.  Values outside it
 * clamp, and NaN/Inf cannot be represented (they clamp too).  That is the
 * trade for a runtime a fraction of the IEEE one: the whole point of making
 * this the default.  Use softfloat32.c (the -soft configs) when the program
 * needs the wider range or more precision. */

typedef union {
    float f;
    uint32_t u;
} f32bits;

static float from_fix(fix16_t v);
static fix16_t to_fix(float f);

/* binary32 bits -> Q16.16, rounding to nearest.  value = M * 2^(e-150), so
 * fixed = M * 2^(e-134). */
static fix16_t fx_from_bits(uint32_t u)
{
    int e = (int)((u >> 23) & 0xFF);
    int neg = (int)(u >> 31);
    uint32_t m = u & 0x007FFFFFu;
    uint32_t sig;
    int sh;
    int32_t v;

    if (e == 0xFF)                              /* inf / nan */
        return neg ? FIX16_MIN : FIX16_MAX;
    if (e == 0)                                 /* subnormal or zero */
        return 0;                               /* below 1/65536 */
    sig = m | 0x00800000u;
    sh = e - 134;
    if (sh >= 0) {
        if (sh >= 8)                            /* >= 2^31 in Q16.16 */
            return neg ? FIX16_MIN : FIX16_MAX;
        v = (int32_t)(sig << sh);
    } else {
        int k = -sh;

        if (k > 24)
            return 0;
        v = (int32_t)((sig + (1u << (k - 1))) >> k);
    }
    return neg ? (fix16_t)((uint32_t)0 - (uint32_t)v) : (fix16_t)v;
}

/* Q16.16 -> binary32 bits.  The magnitude is normalised so the leading one
 * sits at bit 31, then the top 24 bits become the significand; the low eight
 * are the rounding bits. */
static uint32_t fx_to_bits(fix16_t v)
{
    uint32_t sign = 0, mag, n, m, rem;
    int sh = 0, e;

    if (v < 0) {
        sign = 0x80000000u;
        mag = (uint32_t)0 - (uint32_t)v;
    } else {
        mag = (uint32_t)v;
    }
    if (mag == 0)
        return sign;
    n = mag;
    while (!(n & 0x80000000u)) {
        n <<= 1;
        sh++;
    }
    e = 142 - sh;                               /* 127 + msb - 16 */
    rem = n & 0xFFu;
    m = n >> 8;
    if (rem > 0x80u || (rem == 0x80u && (m & 1u)))
        m++;
    if (m == 0x01000000u) {
        m >>= 1;
        e++;
    }
    if (e >= 0xFF)
        return sign | 0x7F800000u;
    if (e <= 0)
        return sign;                            /* underflow to zero */
    return sign | ((uint32_t)e << 23) | (m & 0x007FFFFFu);
}

static float from_fix(fix16_t v)
{
    f32bits x;

    x.u = fx_to_bits(v);
    return x.f;
}

static fix16_t to_fix(float f)
{
    f32bits x;

    x.f = f;
    return fx_from_bits(x.u);
}

float __addsf3(float a, float b)
{
    int32_t r;

    if (__builtin_add_overflow(to_fix(a), to_fix(b), &r))
        return from_fix(r < 0 ? FIX16_MIN : FIX16_MAX);
    return from_fix((fix16_t)r);
}

float __subsf3(float a, float b)
{
    int32_t r;

    if (__builtin_sub_overflow(to_fix(a), to_fix(b), &r))
        return from_fix(r < 0 ? FIX16_MIN : FIX16_MAX);
    return from_fix((fix16_t)r);
}

float __mulsf3(float a, float b)
{
    return from_fix(fix16_mul(to_fix(a), to_fix(b)));
}

float __divsf3(float a, float b)
{
    return from_fix(fix16_div(to_fix(a), to_fix(b)));
}

float __negsf2(float a)
{
    fix16_t v = to_fix(a);

    return from_fix((fix16_t)((uint32_t)0 - (uint32_t)v));
}

/* ---- comparisons ------------------------------------------------------ */

static int cf(float a, float b)
{
    fix16_t x = to_fix(a), y = to_fix(b);

    return x < y ? -1 : x > y ? 1 : 0;
}

int __eqsf2(float a, float b) { return cf(a, b) == 0 ? 0 : 1; }
int __nesf2(float a, float b) { return cf(a, b) == 0 ? 0 : 1; }
int __ltsf2(float a, float b) { return cf(a, b); }
int __gtsf2(float a, float b) { return cf(a, b); }
int __lesf2(float a, float b) { return cf(a, b) <= 0 ? -1 : 1; }
int __gesf2(float a, float b) { return cf(a, b) >= 0 ? 1 : -1; }
int __unordsf2(float a, float b) { (void)a; (void)b; return 0; }

/* ---- conversions ------------------------------------------------------ */

int __fixsfsi(float a)
{
    return fix16_to_int(to_fix(a));
}

unsigned int __fixunssfsi(float a)
{
    fix16_t v = to_fix(a);

    if (v <= 0)
        return 0;
    return (unsigned int)((uint32_t)v >> 16);
}

float __floatsisf(int a)
{
    if (a > 32767)
        a = 32767;
    if (a < -32768)
        a = -32768;
    return from_fix(fix16_from_int(a));
}

float __floatunsisf(unsigned int a)
{
    if (a > 32767u)
        a = 32767u;
    return from_fix(fix16_from_int((int)a));
}
