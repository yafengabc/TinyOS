/* pio - assemble and drive RP2040 PIO state machines from the console.
 *
 *   pio asm <file> [block] [sm]   assemble, print the words, load them
 *   pio start <block> <sm>
 *   pio stop  <block> <sm>
 *   pio rel   <block> <sm>        stop and hand the pins back
 *   pio tx    <block> <sm> <word>...
 *   pio rx    <block> <sm> <count>
 *
 * The assembler is deliberately on the device: a PIO program is 32 words at
 * most, so editing a file with `edit`, running `pio asm`, and watching a
 * scope is a much shorter loop than rebuilding and reflashing.  Programs
 * that ship embedded use the same syntax through a build-time assembler and
 * load the words with the same syscall.
 *
 * Syntax (lowercase, `;` or `//` starts a comment):
 *
 *   .program name
 *   .side_set N [opt] [pindirs]
 *   .origin N
 *   .pin BASE COUNT
 *   .clkdiv INT [FRAC]
 *   .wrap_target
 *   .wrap
 *   .word 0x1234
 *   label:
 *   jmp [cond,] label        cond: always !x x-- !y y-- x!=y pin !osre
 *   wait POL SRC, IDX [rel]  SRC: gpio pin irq
 *   in SRC, COUNT            SRC: pins x y null isr osr
 *   out DEST, COUNT          DEST: pins x y null pindirs pc isr exec
 *   push [iffull] [block|noblock]
 *   pull [ifempty] [block|noblock]
 *   mov DEST, [invert|reverse] SRC
 *   irq [wait|clear] IDX [rel]
 *   set DEST, VALUE          DEST: pins x y pindirs
 *   nop
 *
 * Any instruction may end with `side N` and/or `[DELAY]`; with .side_set the
 * two share the instruction's five-bit delay field (see the RP2040 datasheet,
 * 3.4 "PIO assembly").
 */

#include "lib.h"
#include "tos.h"
#include "usys.h"

#ifdef TOS_PIO

#define MAX_INSNS   32
#define MAX_LABELS  16
#define SRC_MAX     4096

static char src[SRC_MAX];
static char scan[SRC_MAX];
static uint16_t words[MAX_INSNS];

static int origin;
static int wrap_top = -1;
static int wrap_bottom = -1;
static int ss_count, ss_opt, ss_pindirs;
static int pin_base = -1, pin_count;
static int clkdiv_int = 1, clkdiv_frac;
static int shiftctrl;
static int nwords;

struct label {
    char name[24];
    int addr;
};
static struct label labels[MAX_LABELS];
static int nlabels;

/* ---- small text helpers ------------------------------------------------ */

static int is_sep(int c)
{
    return c == ' ' || c == '\t' || c == ',' || c == '\r';
}

static char *skip_sep(char *p)
{
    while (*p && is_sep(*p))
        p++;
    return p;
}

static char *trim(char *p)
{
    char *e;

    while (*p == ' ' || *p == '\t' || *p == '\r')
        p++;
    e = p + strlen(p);
    while (e > p && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r'))
        *--e = '\0';
    return p;
}

static void strip_comment(char *p)
{
    for (; *p; p++) {
        if (*p == ';' || (p[0] == '/' && p[1] == '/')) {
            *p = '\0';
            return;
        }
    }
}

/* Up to eight whitespace/comma separated tokens.  A [delay] token keeps its
 * brackets so it cannot be confused with a number operand. */
static int split(char *p, char tok[8][24])
{
    int n = 0;

    for (;;) {
        int k = 0;

        p = skip_sep(p);
        if (!*p || n >= 8)
            break;
        if (*p == '[') {
            while (*p && *p != ']' && k < 23)
                tok[n][k++] = *p++;
            if (*p == ']')
                tok[n][k++] = *p++;
        } else {
            while (*p && !is_sep(*p) && k < 23)
                tok[n][k++] = *p++;
        }
        tok[n][k] = '\0';
        if (!k)
            break;
        n++;
    }
    return n;
}

static int number(const char *s, int *out)
{
    int base = 10, v = 0, neg = 0;

    if (*s == '-') {
        neg = 1;
        s++;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    } else if (s[0] == '0' && (s[1] == 'b' || s[1] == 'B')) {
        base = 2;
        s += 2;
    }
    if (!*s)
        return -1;
    for (; *s && !is_sep(*s); s++) {
        int d;

        if (*s >= '0' && *s <= '9')
            d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f')
            d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F')
            d = *s - 'A' + 10;
        else
            return -1;
        if (d >= base)
            return -1;
        v = v * base + d;
    }
    *out = neg ? -v : v;
    return 0;
}

static int expr(const char *s, int *out)
{
    if (number(s, out) == 0)
        return 0;
    for (int i = 0; i < nlabels; i++)
        if (!strcmp(labels[i].name, s)) {
            *out = labels[i].addr;
            return 0;
        }
    return -1;
}

static int label_define(const char *name, int addr)
{
    if (nlabels >= MAX_LABELS)
        return -1;
    for (int i = 0; i < nlabels; i++)
        if (!strcmp(labels[i].name, name))
            return -1;
    {
        int k = 0;

        while (name[k] && k < (int)sizeof(labels[0].name) - 1) {
            labels[nlabels].name[k] = name[k];
            k++;
        }
        labels[nlabels].name[k] = '\0';
    }
    labels[nlabels].addr = addr;
    nlabels++;
    return 0;
}

/* ---- operand tables ---------------------------------------------------- */

static int map2(const char *s, const char *a, int va, const char *b, int vb)
{
    if (!strcmp(s, a))
        return va;
    if (!strcmp(s, b))
        return vb;
    return -1;
}

static int cond_val(const char *s)
{
    static const char *n[] = { "always", "!x", "x--", "!y", "y--",
                               "x!=y", "pin", "!osre" };
    for (int i = 0; i < 8; i++)
        if (!strcmp(s, n[i]))
            return i;
    return -1;
}

static int wait_src(const char *s)
{
    if (!strcmp(s, "gpio")) return 0;
    if (!strcmp(s, "pin"))  return 1;
    if (!strcmp(s, "irq"))  return 2;
    return -1;
}

static int in_src(const char *s)
{
    if (!strcmp(s, "pins")) return 0;
    if (!strcmp(s, "x"))    return 1;
    if (!strcmp(s, "y"))    return 2;
    if (!strcmp(s, "null")) return 3;
    if (!strcmp(s, "isr"))  return 6;
    if (!strcmp(s, "osr"))  return 7;
    return -1;
}

static int out_dest(const char *s)
{
    if (!strcmp(s, "pins"))    return 0;
    if (!strcmp(s, "x"))       return 1;
    if (!strcmp(s, "y"))       return 2;
    if (!strcmp(s, "null"))    return 3;
    if (!strcmp(s, "pindirs")) return 4;
    if (!strcmp(s, "pc"))      return 5;
    if (!strcmp(s, "isr"))     return 6;
    if (!strcmp(s, "exec"))    return 7;
    return -1;
}

static int mov_dest(const char *s)
{
    if (!strcmp(s, "pins")) return 0;
    if (!strcmp(s, "x"))    return 1;
    if (!strcmp(s, "y"))    return 2;
    if (!strcmp(s, "exec")) return 4;
    if (!strcmp(s, "pc"))   return 5;
    if (!strcmp(s, "isr"))  return 6;
    if (!strcmp(s, "osr"))  return 7;
    return -1;
}

static int mov_src(const char *s)
{
    if (!strcmp(s, "pins"))   return 0;
    if (!strcmp(s, "x"))      return 1;
    if (!strcmp(s, "y"))      return 2;
    if (!strcmp(s, "null"))   return 3;
    if (!strcmp(s, "status")) return 5;
    if (!strcmp(s, "isr"))    return 6;
    if (!strcmp(s, "osr"))    return 7;
    return -1;
}

static int set_dest(const char *s)
{
    if (!strcmp(s, "pins"))    return 0;
    if (!strcmp(s, "x"))       return 1;
    if (!strcmp(s, "y"))       return 2;
    if (!strcmp(s, "pindirs")) return 4;
    return -1;
}

/* ---- encoding ---------------------------------------------------------- */

static int emit_delay(int word, int side, int has_side, int delay)
{
    int n = ss_count;

    if (delay < 0)
        delay = 0;
    if (n == 0) {
        if (has_side) {
            printf("pio: side-set used without .side_set\n");
            return -1;
        }
        if (delay > 31) {
            printf("pio: delay %d does not fit in 5 bits\n", delay);
            return -1;
        }
        return word | (delay << 8);
    }
    {
        int dmax = (1 << (5 - n)) - 1;
        int sbits = ss_opt ? n - 1 : n;
        int ss;

        if (delay > dmax) {
            printf("pio: delay %d does not fit with side_set %d\n", delay, n);
            return -1;
        }
        if (sbits < 1) {
            printf("pio: .side_set %d opt needs at least 2 bits\n", n);
            return -1;
        }
        if (has_side && (side < 0 || side >= (1 << sbits))) {
            printf("pio: side %d does not fit in %d bit(s)\n", side, sbits);
            return -1;
        }
        /* The 5-bit delay/side-set field carries the side-set value in its most
         * significant bits and the delay in the low (5 - n) bits.  With `opt`
         * the top side-set bit means "drive the pins this cycle", so the value
         * itself is one bit narrower. */
        if (ss_opt)
            ss = (has_side ? (1 << (n - 1)) : 0) | (side & ((1 << sbits) - 1));
        else
            ss = has_side ? (side & ((1 << n) - 1)) : 0;
        return word | (((ss << (5 - n)) | delay) << 8);
    }
}

/* Assemble one instruction line.  Returns the 16-bit word or -1. */
static int encode_insn(const char *mnemonic, char tok[8][24], int ntok)
{
    int side = 0, has_side = 0, delay = 0;
    char op[8][24];
    int nop = 0;
    int word = -1;

    /* Pull `side N` and `[delay]` out of the operand list. */
    for (int i = 0; i < ntok; i++) {
        if (!strcmp(tok[i], "side")) {
            if (i + 1 >= ntok || number(tok[i + 1], &side) < 0) {
                printf("pio: side needs a number\n");
                return -1;
            }
            has_side = 1;
            i++;
        } else if (tok[i][0] == '[') {
            int d = 0, k = 0;
            char b[12];

            for (const char *p = tok[i] + 1; *p && *p != ']' && k < 11; p++)
                b[k++] = *p;
            b[k] = '\0';
            if (number(b, &d) < 0 || d < 0 || d > 31) {
                printf("pio: bad delay '%s'\n", tok[i]);
                return -1;
            }
            delay = d;
        } else if (nop < 8) {
            strcpy(op[nop++], tok[i]);
        }
    }

    if (!strcmp(mnemonic, "jmp")) {
        int cond = 0, target;

        if (nop == 1) {
            if (expr(op[0], &target) < 0)
                goto bad;
        } else if (nop == 2) {
            cond = cond_val(op[0]);
            if (cond < 0 || expr(op[1], &target) < 0)
                goto bad;
        } else {
            goto bad;
        }
        if (target < 0 || target > 31)
            goto bad;
        word = (cond << 5) | target;

    } else if (!strcmp(mnemonic, "wait")) {
        int pol, src, idx;

        if (nop < 3 || number(op[0], &pol) < 0 || (pol != 0 && pol != 1))
            goto bad;
        src = wait_src(op[1]);
        if (src < 0 || number(op[2], &idx) < 0)
            goto bad;
        word = 0x2000 | (pol << 7) | (src << 5);
        if (src == 2) {
            if (idx < 0 || idx > 7)
                goto bad;
            if (nop >= 4 && !strcmp(op[3], "rel"))
                idx |= 0x10;
            word |= idx;
        } else {
            if (idx < 0 || idx > 31)
                goto bad;
            word |= idx;
        }

    } else if (!strcmp(mnemonic, "in")) {
        int src, cnt = 1;

        if (nop < 1)
            goto bad;
        src = in_src(op[0]);
        if (src < 0)
            goto bad;
        if (nop >= 2 && number(op[1], &cnt) < 0)
            goto bad;
        if (cnt < 1 || cnt > 32)
            goto bad;
        word = 0x4000 | (src << 5) | (cnt & 0x1f);

    } else if (!strcmp(mnemonic, "out")) {
        int dest, cnt = 1;

        if (nop < 1)
            goto bad;
        dest = out_dest(op[0]);
        if (dest < 0)
            goto bad;
        if (nop >= 2 && number(op[1], &cnt) < 0)
            goto bad;
        if (cnt < 1 || cnt > 32)
            goto bad;
        word = 0x6000 | (dest << 5) | (cnt & 0x1f);

    } else if (!strcmp(mnemonic, "push") || !strcmp(mnemonic, "pull")) {
        int block = 1, flag = 0;

        for (int i = 0; i < nop; i++) {
            if (!strcmp(op[i], "block"))
                block = 1;
            else if (!strcmp(op[i], "noblock"))
                block = 0;
            else if (!strcmp(op[i], "iffull") || !strcmp(op[i], "ifempty"))
                flag = 1;
            else
                goto bad;
        }
        word = (mnemonic[0] == 'p' && mnemonic[1] == 'u' && mnemonic[3] == 'h')
                   ? 0x8000 : 0x8080;
        word |= (flag << 6) | (block << 5);

    } else if (!strcmp(mnemonic, "mov")) {
        int dest, src, mop = 0;

        if (nop < 2)
            goto bad;
        dest = mov_dest(op[0]);
        if (dest < 0)
            goto bad;
        if (!strcmp(op[1], "invert")) {
            mop = 1;
            if (nop < 3)
                goto bad;
            src = mov_src(op[2]);
        } else if (!strcmp(op[1], "reverse")) {
            mop = 3;
            if (nop < 3)
                goto bad;
            src = mov_src(op[2]);
        } else {
            src = mov_src(op[1]);
        }
        if (src < 0)
            goto bad;
        word = 0xa000 | (dest << 5) | (mop << 3) | src;

    } else if (!strcmp(mnemonic, "irq")) {
        int clr = 0, wait = 0, idx = 0, rel = 0, i = 0;

        if (nop < 1)
            goto bad;
        if (!strcmp(op[0], "wait")) {
            wait = 1;
            i = 1;
        } else if (!strcmp(op[0], "clear")) {
            clr = 1;
            i = 1;
        }
        if (i >= nop || number(op[i], &idx) < 0 || idx < 0 || idx > 7)
            goto bad;
        i++;
        if (i < nop && !strcmp(op[i], "rel"))
            rel = 1;
        word = 0xc000 | (clr << 6) | (wait << 5) | (rel << 4) | idx;

    } else if (!strcmp(mnemonic, "set")) {
        int dest, val;

        if (nop < 2)
            goto bad;
        dest = set_dest(op[0]);
        if (dest < 0 || number(op[1], &val) < 0 || val < 0 || val > 31)
            goto bad;
        word = 0xe000 | (dest << 5) | val;

    } else if (!strcmp(mnemonic, "nop")) {
        word = 0xa042;                 /* mov y, y */

    } else {
        printf("pio: unknown instruction '%s'\n", mnemonic);
        return -1;
    }

    return emit_delay(word, side, has_side, delay);

bad:
    printf("pio: bad operands for %s\n", mnemonic);
    return -1;
}

/* ---- two passes over the source ---------------------------------------- */

static int scan_labels(char *text)
{
    int addr = origin;
    char *p = text;

    while (p && *p) {
        char *nl = strchr(p, '\n');
        char *line;

        if (nl)
            *nl = '\0';
        line = trim(p);
        p = nl ? nl + 1 : NULL;
        strip_comment(line);
        line = trim(line);
        if (!*line)
            continue;

        if (line[0] == '.') {
            if (!strncmp(line, ".origin", 7)) {
                int v;

                if (number(skip_sep(line + 7), &v) == 0)
                    addr = v;
            } else if (!strncmp(line, ".word", 5)) {
                addr++;
            }
            continue;
        }
        if (line[strlen(line) - 1] == ':') {
            line[strlen(line) - 1] = '\0';
            line = trim(line);
            if (label_define(line, addr) < 0) {
                printf("pio: duplicate/overflow label '%s'\n", line);
                return -1;
            }
            continue;
        }
        addr++;
    }
    return 0;
}

static int assemble(char *text)
{
    int addr = origin;
    char *p = text;

    while (p && *p) {
        char *nl = strchr(p, '\n');
        char *line, tok[8][24];
        int ntok;

        if (nl)
            *nl = '\0';
        line = trim(p);
        p = nl ? nl + 1 : NULL;
        strip_comment(line);
        line = trim(line);
        if (!*line)
            continue;

        if (line[0] == '.') {
            char *arg = skip_sep(line + 1);
            int k = 0;

            while (arg[k] && !is_sep(arg[k]))
                k++;
            if (!strncmp(line, ".program", 8)) {
                /* name only */
            } else if (!strncmp(line, ".side_set", 9)) {
                int a = 0, b = 0;

                arg = skip_sep(line + 9);
                if (number(arg, &a) < 0 || a < 0 || a > 5) {
                    printf("pio: bad .side_set\n");
                    return -1;
                }
                ss_count = a;
                ss_opt = 0;
                ss_pindirs = 0;
                while (*arg && !is_sep(arg[0]))
                    arg++;
                for (;;) {
                    arg = skip_sep(arg);
                    if (!*arg)
                        break;
                    if (!strncmp(arg, "opt", 3))
                        ss_opt = 1;
                    else if (!strncmp(arg, "pindirs", 7))
                        ss_pindirs = 1;
                    else
                        break;
                    while (*arg && !is_sep(arg[0]))
                        arg++;
                }
                (void)b;
            } else if (!strncmp(line, ".origin", 7)) {
                int v;

                if (number(skip_sep(line + 7), &v) < 0 || v < 0 || v > 31)
                    return -1;
                if (addr != origin) {
                    printf("pio: .origin must precede the code\n");
                    return -1;
                }
                origin = v;
                addr = v;
            } else if (!strncmp(line, ".pin", 4)) {
                char *q = skip_sep(line + 4);
                int a, c;

                if (number(q, &a) < 0)
                    return -1;
                while (*q && !is_sep(*q))
                    q++;
                q = skip_sep(q);
                if (number(q, &c) < 0 || c < 1 || c > 32)
                    return -1;
                pin_base = a;
                pin_count = c;
            } else if (!strncmp(line, ".clkdiv", 7)) {
                char *q = skip_sep(line + 7);
                int a, c = 0;

                if (number(q, &a) < 0 || a < 1 || a > 65535)
                    return -1;
                while (*q && !is_sep(*q))
                    q++;
                q = skip_sep(q);
                if (*q && number(q, &c) < 0)
                    return -1;
                clkdiv_int = a;
                clkdiv_frac = c;
            } else if (!strncmp(line, ".shiftctrl", 10)) {
                int v;

                if (number(skip_sep(line + 10), &v) < 0)
                    return -1;
                shiftctrl = v;
            } else if (!strncmp(line, ".wrap_target", 12)) {
                wrap_bottom = addr;
            } else if (!strncmp(line, ".wrap", 5)) {
                wrap_top = addr - 1;
            } else if (!strncmp(line, ".word", 5)) {
                int v;

                if (nwords >= MAX_INSNS || number(skip_sep(line + 5), &v) < 0)
                    return -1;
                words[nwords++] = (uint16_t)v;
                addr++;
            } else {
                printf("pio: unknown directive %s\n", line);
                return -1;
            }
            continue;
        }

        if (line[strlen(line) - 1] == ':')
            continue;

        {
            char *q = line;
            char mnemonic[24];
            int k = 0;

            while (*q && !is_sep(*q) && k < 23)
                mnemonic[k++] = *q++;
            mnemonic[k] = '\0';
            ntok = split(q, tok);
            if (nwords >= MAX_INSNS) {
                printf("pio: more than %d instructions\n", MAX_INSNS);
                return -1;
            }
            int w = encode_insn(mnemonic, tok, ntok);

            if (w < 0) {
                printf("pio: at %s\n", line);
                return -1;
            }
            words[nwords++] = (uint16_t)w;
            addr++;
        }
    }
    return 0;
}

/* ---- device calls ------------------------------------------------------ */

static int pio_call(int op, struct pio_req *q)
{
    return tos_sys2(SYS_PIO, op, (int)q);
}

static int do_asm(const char *path, int block, int sm)
{
    struct pio_req q;
    int fd, n;

    fd = open(path);
    if (fd < 0) {
        printf("pio: cannot open %s\n", path);
        return 1;
    }
    n = read(fd, src, SRC_MAX - 1);
    close(fd);
    if (n <= 0) {
        printf("pio: %s is empty\n", path);
        return 1;
    }
    src[n] = '\0';

    /* pass 1 needs its own copy: it terminates every line in place. */
    memcpy(scan, src, (unsigned)(n + 1));
    origin = 0;
    wrap_top = -1;
    wrap_bottom = -1;
    ss_count = 0;
    ss_opt = 0;
    ss_pindirs = 0;
    pin_base = -1;
    pin_count = 0;
    clkdiv_int = 1;
    clkdiv_frac = 0;
    shiftctrl = 0;
    nlabels = 0;
    nwords = 0;

    if (scan_labels(scan) < 0 || assemble(src) < 0)
        return 1;
    if (!nwords) {
        printf("pio: no instructions in %s\n", path);
        return 1;
    }

    printf("pio: %d word(s), origin %d, wrap %d..%d, side_set %d%s%s, "
           "clkdiv %d+%d/256, pins %s%d..%d\n",
           nwords, origin,
           wrap_top >= 0 ? wrap_top : origin + nwords - 1,
           wrap_bottom >= 0 ? wrap_bottom : origin,
           ss_count, ss_opt ? " opt" : "", ss_pindirs ? " pindirs" : "",
           clkdiv_int, clkdiv_frac,
           pin_base >= 0 ? "gp" : "none ",
           pin_base >= 0 ? pin_base : 0,
           pin_base >= 0 ? pin_base + pin_count - 1 : -1);
    for (int i = 0; i < nwords; i++)
        printf("  %04x: %04x\n", origin + i, words[i]);

    memset(&q, 0, sizeof(q));
    q.op = PIO_LOAD;
    q.block = block;
    q.sm = sm;
    q.buf = words;
    q.len = nwords;
    q.origin = origin;
    q.wrap_top = wrap_top >= 0 ? wrap_top : origin + nwords - 1;
    q.wrap_bottom = wrap_bottom >= 0 ? wrap_bottom : origin;
    q.side_set = ss_count;
    q.flags = (ss_opt ? PIO_F_SIDE_OPT : 0) |
              (ss_pindirs ? PIO_F_SIDE_PINDIR : 0);
    q.clkdiv_int = clkdiv_int;
    q.clkdiv_frac = clkdiv_frac;
    q.shiftctrl = shiftctrl;
    q.pin_base = pin_base;
    q.pin_count = pin_count;
    if (pio_call(PIO_LOAD, &q) < 0) {
        puts("pio: assembled, but not loaded (no PIO here, or the SM is busy)\n");
        return 1;
    }
    printf("pio: loaded into pio%d sm%d\n", block, sm);
    return 0;
}

static void usage(void)
{
    puts("usage: pio asm <file> [block] [sm]\n"
         "       pio start|stop|rel <block> <sm>\n"
         "       pio tx <block> <sm> <word>...   (hex or decimal)\n"
         "       pio rx <block> <sm> <count>\n");
}

int main(int argc, char *argv[])
{
    struct pio_req q;
    int block, sm, v;

    if (argc < 2) {
        usage();
        return 0;
    }

    memset(&q, 0, sizeof(q));

    if (!strcmp(argv[1], "asm")) {
        if (argc < 3) {
            usage();
            return 1;
        }
        block = argc > 3 ? atoi(argv[3]) : 0;
        sm = argc > 4 ? atoi(argv[4]) : 0;
        return do_asm(argv[2], block, sm);
    }

    if (argc < 4) {
        usage();
        return 1;
    }
    block = atoi(argv[2]);
    sm = atoi(argv[3]);
    q.block = block;
    q.sm = sm;

    if (!strcmp(argv[1], "start") || !strcmp(argv[1], "stop") ||
        !strcmp(argv[1], "rel")) {
        int op = argv[1][0] == 's' && argv[1][1] == 't' && argv[1][2] == 'a'
                     ? PIO_START
                     : (argv[1][0] == 'r' ? PIO_RELEASE : PIO_STOP);

        if (pio_call(op, &q) < 0) {
            puts("pio: failed (not owner, or no PIO)\n");
            return 1;
        }
        puts("pio: ok\n");
        return 0;
    }

    if (!strcmp(argv[1], "tx")) {
        uint32_t w[8];
        int n = 0;

        for (int i = 4; i < argc && n < 8; i++) {
            int x;

            if (number(argv[i], &x) < 0) {
                printf("pio: bad word '%s'\n", argv[i]);
                return 1;
            }
            w[n++] = (uint32_t)x;
        }
        if (!n) {
            usage();
            return 1;
        }
        q.buf = w;
        q.len = n;
        v = pio_call(PIO_TX, &q);
        printf("pio: wrote %d word(s)\n", v);
        return v < 0 ? 1 : 0;
    }

    if (!strcmp(argv[1], "rx")) {
        uint32_t w[16];
        int want = argc > 4 ? atoi(argv[4]) : 1;

        if (want < 1 || want > 16)
            want = 16;
        q.buf = w;
        q.len = want;
        v = pio_call(PIO_RX, &q);
        if (v < 0) {
            puts("pio: rx failed\n");
            return 1;
        }
        printf("pio: read %d word(s):", v);
        for (int i = 0; i < v; i++)
            printf(" %08x", w[i]);
        putchar('\n');
        return 0;
    }

    usage();
    return 1;
}

#else

int main(void)
{
    puts("pio: this board has no PIO support\n");
    return 0;
}

#endif
