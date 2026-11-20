/* Pawn float/console/argument natives for AVM.  Same behavior as the pvm
 * extras; only the VM-facing string/address helpers differ. */
#include "lib.h"
#include "avm.h"
#include "softfloat32.h"
#include <string.h>

extern int avm_script_argc;
extern char **avm_script_argv;

static float cf(acell c)
{
    float f;
    memcpy(&f, &c, sizeof f);
    return f;
}

static acell fc(float f)
{
    acell c;
    memcpy(&c, &f, sizeof c);
    return c;
}

static acell n_float(avm *vm, acell *p)
{
    (void)vm;
    return fc((float)p[1]);
}

static acell n_strfloat(avm *vm, acell *p)
{
    char buf[48];
    const char *s;
    float sign = 1.0f, v = 0.0f, scale = 0.1f;
    int seen = 0;

    avm_getstring(buf, (const acell *)avm_address(vm, p[1]), sizeof buf);
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

static acell n_floatadd(avm *vm, acell *p)
{
    (void)vm;
    return fc(cf(p[1]) + cf(p[2]));
}

static acell n_floatsub(avm *vm, acell *p)
{
    (void)vm;
    return fc(cf(p[1]) - cf(p[2]));
}

static acell n_floatmul(avm *vm, acell *p)
{
    (void)vm;
    return fc(cf(p[1]) * cf(p[2]));
}

static acell n_floatdiv(avm *vm, acell *p)
{
    (void)vm;
    return fc(cf(p[1]) / cf(p[2]));
}

static acell n_floatabs(avm *vm, acell *p)
{
    (void)vm;
    return p[1] & 0x7fffffff;
}

static acell n_floatcmp(avm *vm, acell *p)
{
    float a = cf(p[1]), b = cf(p[2]);
    (void)vm;
    return a < b ? -1 : (a > b ? 1 : 0);
}

static float floor_f(float x)
{
    float t = (float)(int)x;
    if (x < 0.0f && x != t)
        t -= 1.0f;
    return t;
}

static acell n_floatfract(avm *vm, acell *p)
{
    float x = cf(p[1]);
    (void)vm;
    return fc(x - floor_f(x));
}

static acell n_floatround(avm *vm, acell *p)
{
    float x = cf(p[1]);
    int method = (int)p[2];
    (void)vm;
    switch (method) {
    case 1: return (acell)floor_f(x);
    case 2: {
        float f = floor_f(x);
        if (f != x) f += 1.0f;
        return (acell)f;
    }
    case 3: return (acell)x;
    default: return (acell)floor_f(x + 0.5f);
    }
}

static acell n_floatint(avm *vm, acell *p)
{
    (void)vm;
    return (acell)cf(p[1]);
}

static acell n_floatsqroot(avm *vm, acell *p)
{
    float x = cf(p[1]), y;
    (void)vm;
    if (x <= 0.0f)
        return fc(0.0f);
    y = x > 1.0f ? x : 1.0f;
    for (int i = 0; i < 16; i++)
        y = 0.5f * (y + x / y);
    return fc(y);
}

static acell n_floatpower(avm *vm, acell *p)
{
    float base = cf(p[1]), e = cf(p[2]), r = 1.0f;
    int n = (int)e;
    (void)vm;
    if ((float)n != e)
        return fc(0.0f);
    if (n < 0) {
        base = 1.0f / base;
        n = -n;
    }
    while (n-- > 0)
        r *= base;
    return fc(r);
}

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

static acell n_printf(avm *vm, acell *p)
{
    char fmt[192], buf[64];
    const char *s;
    int total = 0;
    int arg = 2;
    int nargs = (int)p[0] / 4;

    avm_getstring(fmt, (const acell *)avm_address(vm, p[1]), sizeof fmt);
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
            int v = (int)*(const acell *)avm_address(vm, p[arg++]);
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
            uint32_t v = (uint32_t)*(const acell *)avm_address(vm, p[arg++]);
            int n = fmt_uint(buf, v, conv == 'u' ? 10 : 16, conv == 'X');
            if (!left) pad(width - n, zero);
            total += out(buf, n);
            if (left) pad(width - n, 0);
        } else if (conv == 's') {
            char sbuf[128];
            const acell *sp = (const acell *)avm_address(vm, p[arg++]);
            int n = avm_getstring(sbuf, sp, sizeof sbuf);
            if (prec >= 0 && n > prec) n = prec;
            if (!left) pad(width - n, 0);
            total += out(sbuf, n);
            if (left) pad(width - n, 0);
        } else if (conv == 'c') {
            char c = (char)*(const acell *)avm_address(vm, p[arg++]);
            if (!left) pad(width - 1, 0);
            total += out(&c, 1);
            if (left) pad(width - 1, 0);
        } else if (conv == 'f') {
            float v = cf(*(const acell *)avm_address(vm, p[arg++]));
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

static acell n_putchar(avm *vm, acell *p)
{
    char c = (char)p[1];
    (void)vm;
    write_all(1, &c, 1);
    return 1;
}

static acell n_argcount(avm *vm, acell *p)
{
    (void)vm;
    (void)p;
    return avm_script_argc;
}

static void set_string(acell *dst, const char *src, int pack, int max)
{
    int n = (int)strlen(src);
    if (n > max - 1)
        n = max - 1;
    if (pack) {
        int cells = n / 4 + 1;
        memset(dst, 0, (size_t)cells * 4);
        for (int i = 0; i < n; i++)
            dst[i / 4] |= (acell)(uint8_t)src[i] << (24 - 8 * (i % 4));
    } else {
        for (int i = 0; i < n; i++)
            dst[i] = (uint8_t)src[i];
        dst[n] = 0;
    }
}

static acell n_argindex(avm *vm, acell *p)
{
    int index = (int)p[1];
    int max = (int)p[3];
    int pack = (int)p[4];
    if (index < 0 || index >= avm_script_argc)
        return 0;
    set_string((acell *)avm_address(vm, p[2]), avm_script_argv[index],
               pack, max);
    return 1;
}

static acell n_argstr(avm *vm, acell *p)
{
    (void)vm;
    (void)p;
    return 0;
}

static acell n_argvalue(avm *vm, acell *p)
{
    (void)vm;
    (void)p;
    return 0;
}

const avm_native_info avm_extras_natives[] = {
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
