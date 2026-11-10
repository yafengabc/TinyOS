#include "softfloat32.h"
#include "lib.h"

/* Float printing, shared by both runtimes.
 *
 * The arithmetic here is ordinary C float arithmetic, so it resolves to
 * whichever backend the build selected: IEEE binary32 (softfloat32.c) or
 * Q16.16 (softfix.c).  Nothing in this file decides that; it only formats.
 * printf is left alone. */

typedef union {
    float f;
    uint32_t u;
} f32bits;

static float fp_from_bits(uint32_t u)
{
    f32bits x;

    x.u = u;
    return x.f;
}

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

int float_to_str(char *out, int cap, float v, int decimals)
{
    char frac[8];
    uint32_t bits = fp_to_bits(v);
    uint32_t exp = (bits >> 23) & 0xFF;
    int neg = (int)(bits >> 31);
    int n = 0, i;
    float a, rem;
    uint32_t ip;

    if (cap <= 0)
        return 0;
    if (exp == 0xFF) {
        const char *s = (bits & 0x007FFFFFu) ? "nan" : "inf";

        if (neg && !(bits & 0x007FFFFFu))
            n = fp_putc(out, cap, n, '-');
        while (*s)
            n = fp_putc(out, cap, n, *s++);
        out[n < cap - 1 ? n : cap - 1] = '\0';
        return n;
    }
    if (neg)
        n = fp_putc(out, cap, n, '-');
    if (exp >= 158) {                           /* |v| >= 2^31 */
        const char *s = "big";

        while (*s)
            n = fp_putc(out, cap, n, *s++);
        out[n < cap - 1 ? n : cap - 1] = '\0';
        return n;
    }
    if (decimals < 0)
        decimals = 0;
    if (decimals > 7)
        decimals = 7;

    a = fp_from_bits(bits & 0x7FFFFFFFu);
    ip = __fixunssfsi(a);
    rem = __subsf3(a, __floatsisf((int)ip));

    for (i = 0; i < decimals; i++) {
        int d;

        rem = __mulsf3(rem, 10.0f);
        d = __fixsfsi(rem);
        if (d < 0)
            d = 0;
        if (d > 9)
            d = 9;
        frac[i] = (char)('0' + d);
        rem = __subsf3(rem, __floatsisf(d));
    }
    /* Round the last digit from the first dropped one. */
    if (decimals > 0) {
        rem = __mulsf3(rem, 10.0f);
        if (__fixsfsi(rem) >= 5) {
            i = decimals - 1;
            while (i >= 0) {
                if (frac[i] < '9') {
                    frac[i]++;
                    break;
                }
                frac[i] = '0';
                i--;
            }
            if (i < 0)
                ip++;
        }
    }

    n = fp_put_uint(out, cap, n, ip);
    if (decimals > 0) {
        n = fp_putc(out, cap, n, '.');
        for (i = 0; i < decimals; i++)
            n = fp_putc(out, cap, n, frac[i]);
    }
    out[n < cap - 1 ? n : cap - 1] = '\0';
    return n;
}

int print_float_n(float v, int decimals)
{
    char buf[40];
    int n = float_to_str(buf, sizeof(buf), v, decimals);

    if (n > (int)sizeof(buf) - 1)
        n = (int)sizeof(buf) - 1;
    write_all(1, buf, n);
    return n;
}

int print_float(float v)
{
    return print_float_n(v, 4);
}
