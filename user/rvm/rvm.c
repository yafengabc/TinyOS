/* rvm: run csc's register IR. Format is detected by the RIR2 magic. */
#include "lib.h"

TOS_STACK(8 * 1024);

enum {
    R_CONST, R_MOV, R_ADD, R_SUB, R_MUL, R_DIV,
    R_LT, R_GT, R_EQ, R_JZ, R_JMP, R_RET, R_HALT,
    R_LOADG, R_STOREG, R_LOADA, R_STOREA, R_PRINT,
    R_ARG, R_CALL, R_LOADL, R_STOREL,
    R_INC, R_DEC, R_ADDI, R_SUBI, R_MULI,
    R_JLT, R_JGE, R_JGT, R_JLE, R_JEQ, R_JNE,
    R_JLT_I, R_JGE_I, R_JGT_I, R_JLE_I, R_JEQ_I, R_JNE_I,
    R_STOREA_I, R_NOP, R_PRINTS, R_PRINTN, R_PRINTV,
    R_NATIVE, R_NATIVER, R_LOADD, R_STORED,
    R_SHLI, R_SHRI, R_FMUL, R_FDIV, R_PRINTFIX, R_PRINTFIXV,
    R_BOUNDS, R_PRINTFIXN, R_MOD,
    R_LOADB, R_STOREB, R_STRCPY, R_STRCAT,
    R_STRLEN, R_STRCMP, R_ATOI, R_PRINTSTR,
    R_NE, R_LE, R_GE, R_NATIVEC
};

enum {
    NAT_GPIO_COUNT, NAT_GPIO_MODE, NAT_GPIO_WRITE, NAT_GPIO_READ,
    NAT_ADC_COUNT, NAT_ADC_READ, NAT_PWM_COUNT, NAT_PWM_SET, NAT_PWM_OFF,
    NAT_SLEEP, NAT_UPTIME, NAT_RUN,
    NAT_OPEN, NAT_CREATE, NAT_READ, NAT_WRITE, NAT_CLOSE, NAT_UNLINK,
    NAT_CAPTURE, NAT_READDIR, NAT_ISDIR
};

typedef struct { int op, a, b, c, d; } RIR;

#define NREG    16
#define NFRAME  32
#define NSPILL  16
#define GMAX    512
#define SMAX    1024

/* The instruction array is paged in from the file, so a program is limited by
 * the image, not by how many instructions fit in RAM.  A page is PAGE_IR fixed
 * 20-byte records: a fault is one read of at most 5120 bytes and no record can
 * straddle two pages.  Pages are immutable, so they survive across reps. */
#define PAGE_IR 256
#if defined(__riscv) || defined(__arm__)
#define NSLOT   2
#else
#define NSLOT   8
#endif
#define IRMAX   (1 << 16)      /* header sanity cap, not a RAM budget */

static RIR page[NSLOT][PAGE_IR];
static int page_tag[NSLOT];
static int rir_fd = -1;
static int nir;

/* The page mapped into `cur`, as a pc range [cur_lo, cur_hi).  The dispatch
 * loop tests that range first: a sequential instruction is two subtractions
 * and one compare, and only a jump that leaves the page maps a new one. */
static RIR *cur;
static int cur_lo;
static int cur_hi;
static int g[GMAX];
static char strpool[SMAX];

#define AMAX 512
static uint8_t arena[AMAX];
static uint8_t arena_init[AMAX];
static int arena_size;

/* Bounded string helpers: q<0 is a null string, and reads stop at the arena
 * end even when a char array was never NUL-terminated. */
static int arena_len(int off)
{
    int n = 0;

    if (off < 0 || off >= arena_size)
        return 0;
    while (off + n < arena_size && arena[off + n])
        n++;
    return n;
}

static void arena_print(int off)
{
    int n = arena_len(off);
    int i;

    for (i = 0; i < n; i++)
        putchar(arena[off + i]);
}

static int arena_cmp(int a, int b)
{
    int la = arena_len(a), lb = arena_len(b);
    int n = la < lb ? la : lb;

    for (int i = 0; i < n; i++) {
        unsigned char ca = arena[a + i], cb = arena[b + i];
        if (ca != cb)
            return ca < cb ? -1 : 1;
    }
    return la == lb ? 0 : (la < lb ? -1 : 1);
}

static int arena_atoi(int off)
{
    int n = arena_len(off), i = 0, sign = 1, v = 0;

    while (i < n && (arena[off + i] == ' ' || arena[off + i] == '\t'))
        i++;
    if (i < n && (arena[off + i] == '-' || arena[off + i] == '+')) {
        if (arena[off + i] == '-')
            sign = -1;
        i++;
    }
    while (i < n && arena[off + i] >= '0' && arena[off + i] <= '9') {
        v = v * 10 + (arena[off + i] - '0');
        i++;
    }
    return sign * v;
}

/* strlcpy/strlcat semantics on arena offsets.  cap counts bytes. */
static int arena_copy(int dst, int src, int cap)
{
    int sl = arena_len(src);
    int n, i;

    if (cap > 0 && dst >= 0 && dst < arena_size) {
        n = sl < cap - 1 ? sl : cap - 1;
        for (i = 0; i < n && dst + i < arena_size; i++)
            arena[dst + i] = arena[src + i];
        if (dst + i < arena_size)
            arena[dst + i] = 0;
    }
    return sl;
}

static int arena_cat(int dst, int src, int cap)
{
    int dl = arena_len(dst), sl = arena_len(src);
    int space, copy, i;

    if (cap > dl + 1 && dst >= 0 && dst < arena_size) {
        space = cap - 1 - dl;
        copy = sl < space ? sl : space;
        for (i = 0; i < copy && dst + dl + i < arena_size; i++)
            arena[dst + dl + i] = arena[src + i];
        if (dst + dl + i < arena_size)
            arena[dst + dl + i] = 0;
    }
    return dl + sl;
}

static int fmul16(int a, int b)
{
    uint32_t ua, ub, a0, a1, b0, b1, p0, p1, p2, p3, mid, hi, r;
    int neg = (a < 0) != (b < 0);

    ua = a < 0 ? (uint32_t)0 - (uint32_t)a : (uint32_t)a;
    ub = b < 0 ? (uint32_t)0 - (uint32_t)b : (uint32_t)b;
    a0 = ua & 0xFFFFu; a1 = ua >> 16;
    b0 = ub & 0xFFFFu; b1 = ub >> 16;
    p0 = a0 * b0; p1 = a0 * b1; p2 = a1 * b0; p3 = a1 * b1;
    mid = (p0 >> 16) + (p1 & 0xFFFFu) + (p2 & 0xFFFFu);
    hi = p3 + (p1 >> 16) + (p2 >> 16) + (mid >> 16);
    r = (hi << 16) | (mid & 0xFFFFu);
    return (int)(neg ? (uint32_t)0 - r : r);
}

static int fdiv16(int a, int b)
{
    uint32_t ua, ub, q, r, frac, carry;
    int neg, i;

    if (b == 0)
        return a < 0 ? (int)0x80000000 : 0x7fffffff;
    neg = (a < 0) != (b < 0);
    ua = a < 0 ? (uint32_t)0 - (uint32_t)a : (uint32_t)a;
    ub = b < 0 ? (uint32_t)0 - (uint32_t)b : (uint32_t)b;
    q = ua / ub;
    r = ua % ub;
    if (q > 0x7FFFu)
        return neg ? (int)0x80000000 : 0x7fffffff;
    frac = 0;
    for (i = 0; i < 16; i++) {
        carry = r >> 31;
        r <<= 1;
        if (carry || r >= ub) {
            r -= ub;
            frac |= 1u << (15 - i);
        }
    }
    q = (q << 16) | frac;
    return (int)(neg ? (uint32_t)0 - q : q);
}

static void print_fix(int v, int nl)
{
    unsigned u;
    unsigned ip, dec;
    char buf[5];
    int i, n = 4;

    if (v < 0) { putchar('-'); u = (unsigned)0 - (unsigned)v; }
    else u = (unsigned)v;
    ip = u >> 16;
    dec = (unsigned)((((u & 0xffff) * 10000u) + 32768u) >> 16);
    putint((int)ip);
    putchar('.');
    for (i = 3; i >= 0; i--) { buf[i] = (char)('0' + dec % 10); dec /= 10; }
    while (n > 1 && buf[n - 1] == '0') n--;
    for (i = 0; i < n; i++) putchar(buf[i]);
    if (nl) putchar('\n');
}

static void print_fix_n(int v, int decimals)
{
    static const unsigned p10[5] = { 1u, 10u, 100u, 1000u, 10000u };
    unsigned u, ip, frac;
    int i;

    if (decimals < 0) decimals = 0;
    if (decimals > 4) decimals = 4;
    if (v < 0) { putchar('-'); u = (unsigned)0 - (unsigned)v; }
    else u = (unsigned)v;
    ip = u >> 16;
    frac = (unsigned)((((u & 0xffff) * p10[decimals]) + 32768u) >> 16);
    if (frac >= p10[decimals]) {
        frac -= p10[decimals];
        ip++;
    }
    putint((int)ip);
    if (decimals > 0) {
        putchar('.');
        for (i = decimals - 1; i >= 0; i--)
            putchar((char)('0' + (frac / p10[i]) % 10u));
    }
}

typedef struct {
    int r[NREG];
    int spill[NSPILL];
    int ret_pc;
    int dst;
} Frame;

static Frame frames[NFRAME];
static int argbuf[16];
static int entry_pc;
#define FUNCMAX 256
static int func_pc[FUNCMAX];
static int nfunc;

static int map_page(int pc)
{
    int pg = pc / PAGE_IR;
    int base = pg * PAGE_IR;
    int n = nir - base < PAGE_IR ? nir - base : PAGE_IR;
    int slot = pg & (NSLOT - 1);

    if (page_tag[slot] != pg) {
        if (lseek(rir_fd, 28L + base * 20L, 0) < 0)
            return 0;
        if (read(rir_fd, page[slot], n * 20) != n * 20)
            return 0;
        page_tag[slot] = pg;
    }
    cur = page[slot];
    cur_lo = base;
    cur_hi = base + n;
    return 1;
}

/* rvm deliberately keeps a small window of globals, strings and frames so it
 * fits on a board.  An image that overflows the window is still valid RIR2
 * (csc -> rcc can build it), so name the limit instead of reporting a generic
 * load failure. */
static void limit_error(const char *what, int got, int max)
{
    puts("rvm: ");
    puts(what);
    puts(" ");
    putint(got);
    puts(" exceeds rvm limit ");
    putint(max);
    puts("; use csc -> rcc\n");
}

static int load_rir(const char *path)
{
    uint32_t hdr[7];
    int m, gs, ss, as, i;
    long fsize;
    int rc = -1;

    rir_fd = open(path);
    if (rir_fd < 0)
        return -1;
    if (read(rir_fd, hdr, 28) != 28 || hdr[0] != 0x32524952u)
        goto bad;
    m = (int)hdr[1];
    gs = (int)hdr[2];
    entry_pc = (int)hdr[3];
    nfunc = (int)hdr[4];
    ss = (int)hdr[5];
    as = (int)hdr[6];
    if (m <= 0 || gs < 0 || nfunc <= 0 || ss < 0 || as < 0)
        goto bad;
    if (m > IRMAX || gs > GMAX || nfunc > FUNCMAX || ss > SMAX || as > AMAX) {
        if (m > IRMAX)
            limit_error("instructions", m, IRMAX);
        else if (gs > GMAX)
            limit_error("global words", gs, GMAX);
        else if (nfunc > FUNCMAX)
            limit_error("functions", nfunc, FUNCMAX);
        else if (ss > SMAX)
            limit_error("string pool bytes", ss, SMAX);
        else
            limit_error("arena bytes", as, AMAX);
        rc = -2;
        goto bad;
    }
    /* The instruction array is not read here; only the tail sections are.
     * Refuse a header that claims more bytes than the file holds, so a
     * truncated image still fails at load time rather than mid-run. */
    fsize = lseek(rir_fd, 0, 2);
    if (fsize < 28L + (long)m * 20 + (long)nfunc * 4 + ss + as)
        goto bad;
    if (lseek(rir_fd, 28L + (long)m * 20, 0) < 0)
        goto bad;
    for (i = 0; i < nfunc; i++) {
        uint32_t v;
        if (read(rir_fd, &v, 4) != 4)
            goto bad;
        func_pc[i] = (int)v;
        if (func_pc[i] < 0 || func_pc[i] >= m)
            goto bad;
    }
    if (ss > 0) {
        int rgot = 0;
        while (rgot < ss) {
            int n = read(rir_fd, strpool + rgot, ss - rgot);
            if (n <= 0)
                break;
            rgot += n;
        }
        if (rgot != ss)
            goto bad;
    }
    arena_size = as;
    if (as > 0) {
        int agot = 0;
        while (agot < as) {
            int n = read(rir_fd, arena_init + agot, as - agot);
            if (n <= 0)
                break;
            agot += n;
        }
        if (agot != as)
            goto bad;
    }
    nir = m;
    for (i = 0; i < NSLOT; i++)
        page_tag[i] = -1;
    for (i = 0; i < gs; i++)
        g[i] = 0;
    return m;
bad:
    close(rir_fd);
    rir_fd = -1;
    return rc;
}

int main(int argc, char **argv)
{
    int m, pc, ret = 0, nframe = 0, rep, reps = 1, verbose;
    int t0, t1;
    int *regs, *spillp;

    if (argc < 2) { puts("usage: rvm file [reps]\n"); return 2; }
    if (argc > 2) {
        reps = atoi(argv[2]);
        if (reps < 1) reps = 1;
    }
    verbose = (reps == 1);
    m = load_rir(argv[1]);
    if (m < 0) {
        if (m != -2)            /* -2 already reported a specific limit */
            puts("rvm: bad or missing image\n");
        return 1;
    }

    t0 = uptime();
    for (rep = 0; rep < reps; rep++) {
        memset(frames, 0, sizeof frames);
        memset(g, 0, sizeof g);
        memset(arena, 0, sizeof arena);
        if (arena_size > 0)
            memcpy(arena, arena_init, (size_t)arena_size);
        nframe = 0;
        regs = frames[0].r;
        spillp = frames[0].spill;
        pc = entry_pc;
        for (;;) {
            RIR q;
            int *r;
            if ((unsigned)(pc - cur_lo) >= (unsigned)(cur_hi - cur_lo)) {
                if (pc < 0 || pc >= nir) {
                    puts("rvm: pc out of range\n");
                    return 1;
                }
                if (!map_page(pc)) {
                    puts("rvm: image read failed\n");
                    return 1;
                }
            }
            q = cur[pc - cur_lo];
            pc++;
            r = regs;
            switch (q.op) {
            case R_CONST: r[q.d] = q.a; break;
            case R_MOV: r[q.d] = r[q.a]; break;
            case R_ADD: r[q.d] = r[q.a] + r[q.b]; break;
            case R_SUB: r[q.d] = r[q.a] - r[q.b]; break;
            case R_MUL: r[q.d] = r[q.a] * r[q.b]; break;
            case R_DIV: {
                /* Match the native backends: /0 -> 0, INT_MIN / -1 -> INT_MIN
                 * (the overflow trap the host C compiler raises is not a
                 * portable result).  R_MOD already guards zero above. */
                int b = r[q.b];
                if (!b)
                    r[q.d] = 0;
                else if (r[q.a] == INT32_MIN && b == -1)
                    r[q.d] = INT32_MIN;
                else
                    r[q.d] = r[q.a] / b;
                break;
            }
            case R_MOD: {
                int b = r[q.b];
                if (!b || (r[q.a] == INT32_MIN && b == -1))
                    r[q.d] = 0;
                else
                    r[q.d] = r[q.a] % b;
                break;
            }
            case R_LOADB: {
                int ix = r[q.b];
                if (q.a < 0 || q.a >= arena_size || ix < 0 ||
                    q.a + ix >= arena_size) {
                    puts("arena bounds\n");
                    return 1;
                }
                r[q.d] = (unsigned char)arena[q.a + ix];
                break;
            }
            case R_STOREB: {
                int ix = r[q.b];
                if (q.a < 0 || q.a >= arena_size || ix < 0 ||
                    q.a + ix >= arena_size) {
                    puts("arena bounds\n");
                    return 1;
                }
                arena[q.a + ix] = (unsigned char)r[q.c];
                break;
            }
            case R_STRCPY: r[q.d] = arena_copy(r[q.a], r[q.b], r[q.c]); break;
            case R_STRCAT: r[q.d] = arena_cat(r[q.a], r[q.b], r[q.c]); break;
            case R_STRLEN: r[q.d] = arena_len(r[q.a]); break;
            case R_STRCMP: r[q.d] = arena_cmp(r[q.a], r[q.b]); break;
            case R_ATOI: r[q.d] = arena_atoi(r[q.a]); break;
            case R_PRINTSTR: if (verbose) arena_print(r[q.a]); break;
            case R_LT: r[q.d] = r[q.a] < r[q.b]; break;
            case R_GT: r[q.d] = r[q.a] > r[q.b]; break;
            case R_EQ: r[q.d] = r[q.a] == r[q.b]; break;
            case R_NE: r[q.d] = r[q.a] != r[q.b]; break;
            case R_LE: r[q.d] = r[q.a] <= r[q.b]; break;
            case R_GE: r[q.d] = r[q.a] >= r[q.b]; break;
            case R_JZ: if (r[q.a] == 0) pc = q.b; break;
            case R_JMP: pc = q.a; break;
            case R_RET:
                ret = r[q.a];
                if (nframe == 0) goto done;
                {
                    int rp = frames[nframe].ret_pc;
                    int dst = frames[nframe].dst;
                    nframe--;
                    regs = frames[nframe].r;
                    spillp = frames[nframe].spill;
                    frames[nframe].r[dst] = ret;
                    pc = rp;
                }
                break;
            case R_HALT: goto done;
            case R_LOADG: r[q.d] = g[q.a]; break;
            case R_STOREG: g[q.a] = r[q.b]; break;
            case R_LOADA: {
                int ix = r[q.b];
                if (ix < 0 || ix >= q.c) { puts("bounds\n"); return 1; }
                r[q.d] = g[q.a + ix];
                break;
            }
            case R_STOREA: {
                int ix = r[q.b];
                if (ix < 0 || ix >= q.d) { puts("bounds\n"); return 1; }
                g[q.a + ix] = r[q.c];
                break;
            }
            case R_PRINT: if (verbose) { putint(r[q.a]); putchar('\n'); } break;
            case R_PRINTS: if (verbose) { char *s = strpool + q.a; while (*s) putchar(*s++); } break;
            case R_PRINTN: if (verbose) { int k; for (k = 0; k < q.b; k++) putchar(strpool[q.a + k]); } break;
            case R_PRINTV: if (verbose) putint(r[q.a]); break;
            case R_ARG: argbuf[q.a] = r[q.b]; break;
            case R_CALL:
                if (q.a < 0 || q.a >= nfunc) { puts("rvm: bad call\n"); return 1; }
                if (nframe + 1 >= NFRAME) { puts("rvm: call depth\n"); return 1; }
                nframe++;
                regs = frames[nframe].r;
                spillp = frames[nframe].spill;
                frames[nframe].ret_pc = pc;
                frames[nframe].dst = q.b;
                for (int k = 0; k < q.c && k < 16; k++)
                    frames[nframe].r[k] = argbuf[k];
                pc = func_pc[q.a];
                break;
            case R_LOADL:
                if ((unsigned)q.a >= NSPILL) { puts("spill load out of range\n"); return 1; }
                r[q.d] = spillp[q.a];
                break;
            case R_STOREL:
                if ((unsigned)q.b >= NSPILL) { puts("spill store out of range\n"); return 1; }
                spillp[q.b] = r[q.a];
                break;
            case R_LOADD: r[q.d] = g[r[q.a] + q.b]; break;
            case R_STORED: g[r[q.a] + q.b] = r[q.c]; break;
            case R_SHLI: r[q.d] = r[q.a] << q.b; break;
            case R_SHRI: r[q.d] = r[q.a] >> q.b; break;
            case R_FMUL: r[q.d] = fmul16(r[q.a], r[q.b]); break;
            case R_FDIV: r[q.d] = fdiv16(r[q.a], r[q.b]); break;
            case R_PRINTFIX: if (verbose) print_fix(r[q.a], 1); break;
            case R_PRINTFIXV: if (verbose) print_fix(r[q.a], 0); break;
            case R_PRINTFIXN: if (verbose) print_fix_n(r[q.a], q.b); break;
            case R_BOUNDS:
                if (r[q.a] < 0 || r[q.a] >= q.b) { puts("bounds\n"); return 1; }
                break;
            case R_INC: r[q.d] = r[q.a] + 1; break;
            case R_DEC: r[q.d] = r[q.a] - 1; break;
            case R_ADDI: r[q.d] = r[q.a] + q.b; break;
            case R_SUBI: r[q.d] = r[q.a] - q.b; break;
            case R_MULI: r[q.d] = r[q.a] * q.b; break;
            case R_JLT: if (r[q.a] < r[q.b]) pc = q.c; break;
            case R_JGE: if (r[q.a] >= r[q.b]) pc = q.c; break;
            case R_JGT: if (r[q.a] > r[q.b]) pc = q.c; break;
            case R_JLE: if (r[q.a] <= r[q.b]) pc = q.c; break;
            case R_JEQ: if (r[q.a] == r[q.b]) pc = q.c; break;
            case R_JNE: if (r[q.a] != r[q.b]) pc = q.c; break;
            case R_JLT_I: if (r[q.a] < q.b) pc = q.c; break;
            case R_JGE_I: if (r[q.a] >= q.b) pc = q.c; break;
            case R_JGT_I: if (r[q.a] > q.b) pc = q.c; break;
            case R_JLE_I: if (r[q.a] <= q.b) pc = q.c; break;
            case R_JEQ_I: if (r[q.a] == q.b) pc = q.c; break;
            case R_JNE_I: if (r[q.a] != q.b) pc = q.c; break;
            case R_STOREA_I: {
                int ix = r[q.b];
                if (ix < 0 || ix >= q.d) { puts("bounds\n"); return 1; }
                g[q.a + ix] = q.c;
                break;
            }
            case R_NOP: break;
            case R_NATIVE: {
                int a0 = q.b >= 0 ? r[q.b] : 0;
                int a1 = q.c >= 0 ? r[q.c] : 0;
                int a2 = q.d >= 0 ? r[q.d] : 0;
                switch (q.a) {
                case NAT_GPIO_MODE: gpio_mode(a0, a1); break;
                case NAT_GPIO_WRITE: gpio_write(a0, a1); break;
                case NAT_PWM_SET: pwm_set(a0, a1, a2); break;
                case NAT_PWM_OFF: pwm_off(a0); break;
                case NAT_SLEEP: sleep(a0); break;
                }
                break;
            }
            case R_NATIVER: {
                int a0 = q.b >= 0 ? r[q.b] : 0;
                int a1 = q.c >= 0 ? r[q.c] : 0;
                int res = 0;
                switch (q.a) {
                case NAT_GPIO_COUNT: res = gpio_count(); break;
                case NAT_GPIO_READ: res = gpio_read(a0); break;
                case NAT_ADC_COUNT: res = adc_count(); break;
                case NAT_ADC_READ: res = adc_read(a0); break;
                case NAT_PWM_COUNT: res = pwm_count(); break;
                case NAT_UPTIME: res = uptime(); break;
                case NAT_RUN: {
#if defined(__riscv) || defined(__arm__)
                    int pid;
                    if (a0 < 0 || a0 >= arena_size) {
                        res = -1;
                        break;
                    }
                    pid = exec((const char *)(arena + a0), 0, 0);
                    res = pid < 0 ? pid : wait(pid);
#else
                    res = -1;      /* host build has no child processes */
#endif
                    break;
                }
                }
                r[q.d] = res;
                (void)a1;
                break;
            }
            case R_NATIVEC: {
                int res = -1;
#if defined(__riscv) || defined(__arm__)
                int a0 = argbuf[0], a1 = argbuf[1], a2 = argbuf[2];
                switch (q.a) {
                case NAT_OPEN:
                    if (a0 >= 0 && a0 < arena_size)
                        res = open((const char *)(arena + a0));
                    break;
                case NAT_CREATE:
                    if (a0 >= 0 && a0 < arena_size)
                        res = create((const char *)(arena + a0));
                    break;
                case NAT_READ:
                    if (a0 >= 0 && a1 >= 0 && a2 >= 0 && a1 + a2 <= arena_size)
                        res = read(a0, arena + a1, a2);
                    break;
                case NAT_WRITE:
                    if (a0 >= 0 && a1 >= 0 && a2 >= 0 && a1 + a2 <= arena_size)
                        res = write(a0, arena + a1, a2);
                    break;
                case NAT_CLOSE: res = close(a0); break;
                case NAT_UNLINK:
                    if (a0 >= 0 && a0 < arena_size)
                        res = unlink((const char *)(arena + a0));
                    break;
                case NAT_READDIR:
                    if (a0 >= 0 && a1 >= 0 && a2 > 0 && a1 + a2 <= arena_size)
                        res = readdir_index(a0, (char *)(arena + a1), a2);
                    break;
                case NAT_ISDIR: {
                    struct stat st;
                    if (a0 >= 0 && a0 < arena_size &&
                        stat((const char *)(arena + a0), &st) == 0)
                        res = st.type == 1;      /* TOS_FTYPE_DIR */
                    break;
                }
                case NAT_CAPTURE: {
                    /* run(path) with its stdout wired into a pipe.  The child
                     * inherits fd 1 at exec time, so the parent points fd 1 at
                     * the pipe, spawns, then restores it. */
                    int fds[2];
                    int saved, pid, n = 0;
                    if (a0 < 0 || a0 >= arena_size || a1 < 0 ||
                        a2 < 0 || a1 + a2 > arena_size)
                        break;
                    if (pipe(fds) < 0)
                        break;
                    saved = dup(1);
                    if (saved < 0) {
                        close(fds[0]);
                        close(fds[1]);
                        break;
                    }
                    dup2(fds[1], 1);
                    close(fds[1]);
                    pid = exec((const char *)(arena + a0), 0, 0);
                    dup2(saved, 1);
                    close(saved);
                    if (pid < 0) {
                        close(fds[0]);
                        res = pid;
                        break;
                    }
                    while (n < a2) {
                        int r = read(fds[0], arena + a1 + n, a2 - n);
                        if (r <= 0)
                            break;
                        n += r;
                    }
                    close(fds[0]);
                    wait(pid);
                    res = n;
                    break;
                }
                }
#endif
                if (q.b >= 0) r[q.b] = res;
                break;
            }
            default: puts("rvm: bad op\n"); return 1;
            }
        }
done:
        ;
    }
    t1 = uptime();
    puts("result=");
    putint(ret);
    puts(" ir=");
    putint(m);
    puts(" reps=");
    putint(reps);
    puts(" exec_ms=");
    putint(t1 - t0);
    puts("\n");
    return 0;
}
