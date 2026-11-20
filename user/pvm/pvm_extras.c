/* Pawn float/console/argument natives for pvm.
 *
 * Float arithmetic uses TinyOS's softfloat32 (the same binary32 helpers the
 * C compiler emits on RV32IMC/Cortex-M0+), so standard pawncc Float: values
 * and literals work unchanged.  printf supports the common subset:
 * %d %i %u %x %X %s %c %f %% with width/precision.
 */
#include "lib.h"
#include "amxcfg.h"
#include "amx.h"
#include "softfloat32.h"
#include <string.h>

#if defined(TOS_PVM_FLOAT)

extern int pvm_script_argc;
extern char **pvm_script_argv;

static float cf(cell c)
{
    float f;
    memcpy(&f, &c, sizeof f);
    return f;
}

static cell fc(float f)
{
    cell c;
    memcpy(&c, &f, sizeof c);
    return c;
}

static cell n_float(AMX *amx, const cell *p)
{
    (void)amx;
    return fc((float)p[1]);
}

static cell n_strfloat(AMX *amx, const cell *p)
{
    char buf[48];
    const char *s;
    float sign = 1.0f, v = 0.0f, scale = 0.1f;
    int seen = 0;

    amx_GetString(buf, (const cell *)amx_Address(amx, p[1]), 0, sizeof buf);
    s = buf;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '-') { sign = -1.0f; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9') {
        v = v * 10.0f + (float)(*s - '0');
        s++;
        seen = 1;
    }
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            v += (float)(*s - '0') * scale;
            scale *= 0.1f;
            s++;
            seen = 1;
        }
    }
    return seen ? fc(sign * v) : fc(0.0f);
}

static cell n_floatadd(AMX *amx, const cell *p)
{
    (void)amx;
    return fc(cf(p[1]) + cf(p[2]));
}

static cell n_floatsub(AMX *amx, const cell *p)
{
    (void)amx;
    return fc(cf(p[1]) - cf(p[2]));
}

static cell n_floatmul(AMX *amx, const cell *p)
{
    (void)amx;
    return fc(cf(p[1]) * cf(p[2]));
}

static cell n_floatdiv(AMX *amx, const cell *p)
{
    (void)amx;
    return fc(cf(p[1]) / cf(p[2]));
}

static cell n_floatabs(AMX *amx, const cell *p)
{
    (void)amx;
    return p[1] & 0x7fffffff;
}

static cell n_floatcmp(AMX *amx, const cell *p)
{
    float a = cf(p[1]), b = cf(p[2]);
    (void)amx;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static float floor_f(float x)
{
    float t = (float)(int)x;
    if (x < 0.0f && x != t)
        t -= 1.0f;
    return t;
}

static cell n_floatfract(AMX *amx, const cell *p)
{
    float x = cf(p[1]);
    (void)amx;
    return fc(x - floor_f(x));
}

static cell n_floatround(AMX *amx, const cell *p)
{
    float x = cf(p[1]);
    int method = (int)p[2];
    (void)amx;
    switch (method) {
    case 1: return (cell)floor_f(x);                 /* floor */
    case 2: {                                        /* ceil */
        float f = floor_f(x);
        if (f != x) f += 1.0f;
        return (cell)f;
    }
    case 3: return (cell)x;                          /* truncate */
    default: return (cell)floor_f(x + 0.5f);         /* round */
    }
}

static cell n_floatint(AMX *amx, const cell *p)
{
    (void)amx;
    return (cell)cf(p[1]);
}

static cell n_floatsqroot(AMX *amx, const cell *p)
{
    float x = cf(p[1]), y;
    (void)amx;
    if (x <= 0.0f)
        return fc(0.0f);
    y = x > 1.0f ? x : 1.0f;
    for (int i = 0; i < 16; i++)
        y = 0.5f * (y + x / y);
    return fc(y);
}

static cell n_floatpower(AMX *amx, const cell *p)
{
    float base = cf(p[1]), e = cf(p[2]), r = 1.0f;
    int n = (int)e;
    (void)amx;
    if ((float)n != e)
        return fc(0.0f);          /* fractional exponent unsupported */
    if (n < 0) {
        base = 1.0f / base;
        n = -n;
    }
    while (n-- > 0)
        r *= base;
    return fc(r);
}

/* ---- printf ---------------------------------------------------------- */

static int out(const char *s, int n)
{
    if (n > 0)
        write_all(1, s, n);
    return n;
}

static int fmt_uint(char *buf, uint32_t v, int base, int upper)
{
    static const char lo[] = "0123456789abcdef";
    static const char hi[] = "0123456789ABCDEF";
    const char *digits = upper ? hi : lo;
    char tmp[12];
    int n = 0;

    if (v == 0) {
        buf[0] = '0';
        return 1;
    }
    while (v) {
        tmp[n++] = digits[v % (uint32_t)base];
        v /= (uint32_t)base;
    }
    for (int i = 0; i < n; i++)
        buf[i] = tmp[n - 1 - i];
    return n;
}

static void pad(int n, int zero)
{
    char c = zero ? '0' : ' ';
    while (n-- > 0)
        write_all(1, &c, 1);
}

static int copy_str(char *dst, const cell *src, int cap)
{
    int n = 0;
    if ((uint32_t)*src > 0x00ffffffu) {
        const cell *s = src;
        int idx = 3;
        cell c = 0;
        for (;;) {
            if (idx == 3)
                c = *s++;
            int ch = (int)((c >> (idx * 8)) & 0xff);
            if (ch == 0)
                break;
            if (n < cap - 1)
                dst[n++] = (char)ch;
            idx = (idx + 3) & 3;
        }
    } else {
        while (*src != 0 && n < cap - 1)
            dst[n++] = (char)(*src++);
    }
    dst[n] = '\0';
    return n;
}

static cell n_printf(AMX *amx, const cell *p)
{
    char fmt[192], buf[64];
    const char *s;
    int total = 0;
    int arg = 2;
    int nargs = (int)p[0] / 4;

    amx_GetString(fmt, (const cell *)amx_Address(amx, p[1]), 0, sizeof fmt);
    s = fmt;
    while (*s) {
        if (*s != '%') {
            total += out(s, 1);
            s++;
            continue;
        }
        s++;
        if (*s == '%') {
            total += out("%", 1);
            s++;
            continue;
        }

        int left = 0, zero = 0, plus = 0, space = 0;
        int width = 0, prec = -1;
        for (;; s++) {
            if (*s == '-') left = 1;
            else if (*s == '0') zero = 1;
            else if (*s == '+') plus = 1;
            else if (*s == ' ') space = 1;
            else break;
        }
        while (*s >= '0' && *s <= '9')
            width = width * 10 + (*s++ - '0');
        if (*s == '.') {
            s++;
            prec = 0;
            while (*s >= '0' && *s <= '9')
                prec = prec * 10 + (*s++ - '0');
        }
        while (*s == 'l' || *s == 'h')
            s++;
        if (*s == 0)
            break;
        if (arg > nargs)
            break;

        char conv = *s++;
        if (conv == 'd' || conv == 'i') {
            int v = (int)*(const cell *)amx_Address(amx, p[arg++]);
            uint32_t u;
            int neg = v < 0;
            u = neg ? (uint32_t)(0u - (uint32_t)v) : (uint32_t)v;
            int n = fmt_uint(buf, u, 10, 0);
            if (neg) { memmove(buf + 1, buf, (size_t)n); buf[0] = '-'; n++; }
            else if (plus) { memmove(buf + 1, buf, (size_t)n); buf[0] = '+'; n++; }
            else if (space) { memmove(buf + 1, buf, (size_t)n); buf[0] = ' '; n++; }
            if (!left) pad(width - n, zero);
            total += out(buf, n);
            if (left) pad(width - n, 0);
        } else if (conv == 'u' || conv == 'x' || conv == 'X') {
            uint32_t v = (uint32_t)*(const cell *)amx_Address(amx, p[arg++]);
            int n = fmt_uint(buf, v, conv == 'u' ? 10 : 16, conv == 'X');
            if (!left) pad(width - n, zero);
            total += out(buf, n);
            if (left) pad(width - n, 0);
        } else if (conv == 's') {
            char sbuf[128];
            const cell *sp = (const cell *)amx_Address(amx, p[arg]);
            int n = copy_str(sbuf, sp, sizeof sbuf);
            arg++;
            if (prec >= 0 && n > prec) n = prec;
            if (!left) pad(width - n, 0);
            total += out(sbuf, n);
            if (left) pad(width - n, 0);
        } else if (conv == 'c') {
            char c = (char)*(const cell *)amx_Address(amx, p[arg++]);
            if (!left) pad(width - 1, 0);
            total += out(&c, 1);
            if (left) pad(width - 1, 0);
        } else if (conv == 'f') {
            float v = cf(*(const cell *)amx_Address(amx, p[arg++]));
            int d = prec >= 0 ? prec : 6;
            int n = float_to_str(buf, sizeof buf, v, d);
            if (n > (int)sizeof buf - 1)
                n = (int)sizeof buf - 1;
            if (!left) pad(width - n, 0);
            total += out(buf, n);
            if (left) pad(width - n, 0);
        } else {
            total += out("%", 1);
            total += out(&conv, 1);
        }
    }
    return total;
}

static cell n_putchar(AMX *amx, const cell *p)
{
    char c = (char)p[1];
    (void)amx;
    write_all(1, &c, 1);
    return 1;
}

/* ---- args ------------------------------------------------------------ */

static cell n_argcount(AMX *amx, const cell *p)
{
    (void)amx;
    (void)p;
    return pvm_script_argc;
}

static cell n_argindex(AMX *amx, const cell *p)
{
    int index = (int)p[1];
    int max = (int)p[3];
    int pack = (int)p[4];
    if (index < 0 || index >= pvm_script_argc)
        return 0;
    amx_SetString((cell *)amx_Address(amx, p[2]), pvm_script_argv[index],
                  pack, 0, (size_t)max);
    return 1;
}

static cell n_argstr(AMX *amx, const cell *p)
{
    (void)amx;
    (void)p;
    return 0;
}

static cell n_argvalue(AMX *amx, const cell *p)
{
    (void)amx;
    (void)p;
    return 0;
}

const AMX_NATIVE_INFO pvm_extras_natives[] = {
    { "float", n_float },
    { "strfloat", n_strfloat },
    { "floatadd", n_floatadd },
    { "floatsub", n_floatsub },
    { "floatmul", n_floatmul },
    { "floatdiv", n_floatdiv },
    { "floatabs", n_floatabs },
    { "floatcmp", n_floatcmp },
    { "floatfract", n_floatfract },
    { "floatround", n_floatround },
    { "floatint", n_floatint },
    { "floatsqroot", n_floatsqroot },
    { "floatpower", n_floatpower },
    { "printf", n_printf },
    { "putchar", n_putchar },
    { "argcount", n_argcount },
    { "argindex", n_argindex },
    { "argstr", n_argstr },
    { "argvalue", n_argvalue },
    { NULL, NULL }
};
#endif /* TOS_PVM_FLOAT */
