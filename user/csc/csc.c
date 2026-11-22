/* csc prototype: AST -> RIR -> backend interface.
 * Host prototype, not part of the TinyOS tree yet. */
#include "lib.h"
#include <string.h>
#include <stdint.h>

TOS_STACK(16 * 1024);

#define SRC_MAX 8192
#define NODE_MAX 4096
#define SYM_MAX 64
#define IR_MAX 1024
#define VREG_MAX 256

#if defined(__riscv) || defined(__arm__)
#undef SRC_MAX
#undef NODE_MAX
#undef IR_MAX
#undef VREG_MAX
#define SRC_MAX 4096
#define NODE_MAX 256
#define IR_MAX 256
#define VREG_MAX 24
#endif

typedef struct Node Node;
struct Node {
    uint64_t hash;      /* identifier hash; N_DECL keeps a text-pool offset */
    Node *lhs, *rhs;
    Node *cond, *then, *els, *body, *next;
    Node *args;
    int val;
    uint32_t field_off; /* text-pool offset of a (possibly dotted) field path */
    uint16_t alen;
    uint16_t line;
    uint8_t kind;
    uint8_t stype;
    uint8_t ftype;      /* 1 = Q16.16 float literal/value */
    uint8_t ctype;      /* 1 = char, 2 = char array, 3 = string */
};

enum {
    N_NUM, N_VAR, N_ADD, N_SUB, N_MUL, N_DIV, N_MOD,
    N_LT, N_GT, N_EQ, N_NE, N_LE, N_GE,
    N_AND, N_OR,
    N_ASSIGN, N_DECL, N_RETURN,
    N_BLOCK, N_IF, N_WHILE, N_FUNC, N_FOR, N_DO,
    N_BREAK, N_CONTINUE, N_SWITCH, N_CASE,
    N_INDEX, N_ASSIGN_INDEX, N_PRINT, N_CALL,
    N_STR, N_PRINTF, N_PUTS,
    N_MEMBER, N_ASSIGN_MEMBER,
    N_MEMBER_INDEX, N_ASSIGN_MEMBER_INDEX
};

typedef struct {
    char name[32];
    uint64_t hash;
    int reg;
    int global;
    int offset;
    int is_array;
    int alen;
    int stype;      /* >0 for a struct variable */
    int is_struct;
    int is_param;   /* struct parameter: reg holds the base offset */
    int is_float;
    int is_char;        /* char scalar, still stored in one word */
    int is_char_array;  /* byte array in the string arena */
    int is_string;      /* word holding a string-arena offset */
} Sym;

typedef struct {
    char name[32];
    int nfields;
    char fields[8][32];
    int ftype[8];   /* 0 = int, >0 = nested struct type id */
    int foff[8];    /* word offset inside the struct */
    int size;       /* total words */
} StructType;

#if defined(__riscv) || defined(__arm__)
#define STRUCT_MAX 8
#else
#define STRUCT_MAX 16
#endif
static StructType structs[STRUCT_MAX];
static int nstructs;

typedef struct {
    int op;
    int a, b, c, d;
} RIR;

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

typedef struct {
    int name_off;           /* offset into name_pool */
    int entry_pc;
    signed char nparams;
    signed char rtype;      /* 0 = int, 1 = float, 2 = string */
    signed char rtype_struct; /* >0 when the function returns a struct type */
    signed char ptype[8];   /* 0 = int, -1 = float, -2 = string, >0 = struct */
} Sig;                      /* 20 bytes: 256 functions cost 5 KiB, not 85 */

/* The function table is per file, not per function body: pass 2 re-reads the
 * parameter names from the source, so only the type summary has to survive
 * pass 1.  Storing the whole Func (340 B, 75% of it params[8][32]) capped the
 * toolchain at 32 functions, which UFCS-style code reaches immediately. */
#if defined(__riscv) || defined(__arm__)
#define FUNC_MAX 256
#define NAME_POOL 6144
#else
#define FUNC_MAX 1024
#define NAME_POOL 24576
#endif
static Sig sigs[FUNC_MAX];
static char name_pool[NAME_POOL];
static int name_used;
static int nfuncs;

/* Parameter names of the function being generated (pass 2 only). */
static char cur_params[8][32];

static const char *sig_name(int i)
{
    return name_pool + sigs[i].name_off;
}

#define SBUF 256
static char sbuf[SBUF];
static int slen, spos, seof;
static int src_line = 1;
static int src_fd;
static char tok[64];
static int tok_kind; /* 0 eof, 1 ident, 2 num, 3 punct, 4 string, 5 float */
static int tok_num;
static char strpool[1024];
static int strsize;

/* Mutable byte arena for char arrays and expression string literals.  Its
 * initial contents are written after the string pool; the VM keeps both the
 * initial image and a working copy so benchmark repetitions start clean. */
#define ARENA_MAX 512
static uint8_t arena[ARENA_MAX];
static int arena_next;

static Node nodes[NODE_MAX];
static int nnode;
static Sym syms[SYM_MAX];
static int nsyms;
#define CONST_MAX 64
typedef struct {
    char name[32];
    int val;
} Const;
static Const consts[CONST_MAX];
static int nconsts;
static int nlocals;
static int nglobals;
static int gdata_next;
#define GMAX 512            /* rvm's fixed global word array */

static RIR ir[IR_MAX];
static int nir;
static int vreg;
static int last_type;
static int cur_rtype;
static int cur_rstruct;
static int cur_ret_base = -1;

static void die(const char *s)
{
    puts("csc: ");
    puts(s);
    puts(" at line ");
    putint(src_line);
    puts("\n");
    exit(1);
}

static void die_node(Node *n, const char *s)
{
    puts("csc: ");
    puts(s);
    puts(" at line ");
    putint(n ? n->line : src_line);
    puts("\n");
    exit(1);
}

static int src_fill(void)
{
    if (spos > 0) {
        memmove(sbuf, sbuf + spos, (size_t)(slen - spos));
        slen -= spos;
        spos = 0;
    }
    if (slen >= SBUF)
        return 0;
    {
        int n = read(src_fd, sbuf + slen, SBUF - slen);
        if (n <= 0) {
            seof = 1;
            return 0;
        }
        slen += n;
    }
    return 1;
}

static int src_peek(int k)
{
    while (spos + k >= slen && !seof)
        if (!src_fill())
            break;
    return (spos + k < slen) ? (unsigned char)sbuf[spos + k] : -1;
}

static int src_get(void)
{
    int c = src_peek(0);
    if (c >= 0) {
        spos++;
        if (c == '\n')
            src_line++;
    }
    return c;
}

static void src_rewind(void)
{
    lseek(src_fd, 0, 0);
    slen = 0;
    spos = 0;
    seof = 0;
    src_line = 1;
}

static void next(void)
{
    int c;
    /* Windows editors save UTF-8 with a BOM; skip it once at file start
     * instead of reporting "int expected at line 1" (issue #5). */
    if (spos == 0 && src_peek(0) == 0xEF && src_peek(1) == 0xBB &&
        src_peek(2) == 0xBF) {
        src_get(); src_get(); src_get();
    }
    for (;;) {
        c = src_peek(0);
        if (c < 0) { tok_kind = 0; tok[0] = 0; return; }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { src_get(); continue; }
        if (c == '/' && src_peek(1) == '/') {
            src_get(); src_get();
            while ((c = src_peek(0)) >= 0 && c != '\n') src_get();
            continue;
        }
        if (c == '/' && src_peek(1) == '*') {
            src_get(); src_get();
            while ((c = src_peek(0)) >= 0) {
                if (c == '*' && src_peek(1) == '/') { src_get(); src_get(); break; }
                src_get();
            }
            continue;
        }
        break;
    }
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        int n = 0;
        while ((c = src_peek(0)) >= 0) {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_')) break;
            if (n < (int)sizeof(tok) - 1) tok[n++] = (char)c;
            src_get();
        }
        tok[n] = 0; tok_kind = 1;
    } else if (c >= '0' && c <= '9') {
        long long v = 0;
        while ((c = src_peek(0)) >= 0 && c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
            if (v > 2147483647LL)
                die("integer literal out of range");
            src_get();
        }
        if (src_peek(0) == '.' && src_peek(1) >= '0' && src_peek(1) <= '9') {
            int frac = 0, scale = 1, fd = 0;
            src_get();
            while ((c = src_peek(0)) >= 0 && c >= '0' && c <= '9') {
                if (fd < 4) {
                    frac = frac * 10 + (c - '0');
                    scale *= 10;
                    fd++;
                }
                src_get();
            }
            if (v > 32767)
                die("float literal out of range (Q16.16 max 32767)");
            tok_num = (int)v * 65536 + (frac * 65536 + scale / 2) / scale;
            tok_kind = 5;
        } else {
            tok_num = v;
            tok_kind = 2;
        }
    } else if (c == '"') {
        int start = strsize;
        src_get();
        while ((c = src_peek(0)) >= 0 && c != '"') {
            char ch = (char)src_get();
            if (ch == '\\') {
                int e = src_get();
                if (e == 'n') ch = '\n';
                else if (e == 't') ch = '\t';
                else if (e == 'r') ch = '\r';
                else if (e >= 0) ch = (char)e;
            }
            if (strsize >= (int)sizeof(strpool) - 1)
                die("string pool full");
            strpool[strsize++] = ch;
        }
        if (src_peek(0) == '"') src_get();
        if (strsize < (int)sizeof(strpool))
            strpool[strsize++] = 0;
        tok_num = start;
        tok_kind = 4;
    } else if (c == '\'') {
        int v;
        src_get();
        if (src_peek(0) < 0) die("unterminated character literal");
        v = (unsigned char)src_get();
        if (v == '\\') {
            int e = src_get();
            if (e == 'n') v = '\n';
            else if (e == 't') v = '\t';
            else if (e == 'r') v = '\r';
            else if (e == '0') v = 0;
            else if (e >= 0) v = e;
        }
        if (src_peek(0) == '\'') src_get();
        tok_num = v;
        tok_kind = 2;
    } else {
        char a = (char)src_get();
        char b = (char)src_peek(0);
        int two = 0;
        if ((a == '=' || a == '!' || a == '<' || a == '>') && b == '=')
            two = 1;
        else if ((a == '+' || a == '-' || a == '*' || a == '/' || a == '%') && b == '=')
            two = 1;
        else if ((a == '+' || a == '-') && b == a)
            two = 1;
        else if ((a == '&' || a == '|') && b == a)
            two = 1;
        if (two) {
            tok[0] = a; tok[1] = b; tok[2] = 0; src_get();
        } else { tok[0] = a; tok[1] = 0; }
        tok_kind = 3;
    }
}

static int accept(const char *s)
{
    if (tok_kind == 3 && !strcmp(tok, s)) { next(); return 1; }
    return 0;
}

static void expect(const char *s)
{
    if (!accept(s)) { puts("csc: expected "); puts(s); puts("\n"); exit(1); }
}

static int is_kw(const char *s) { return tok_kind == 1 && !strcmp(tok, s); }

/* 64-bit FNV-1a.  Nodes carry only this hash; the symbol, function, struct and
 * constant tables keep the real names, so a collision between two names is
 * detected where they are inserted rather than silently merging two symbols.
 * With the tables bounded by FUNC_MAX/SYM_MAX/STRUCT_MAX the chance is ~1e-14
 * for a whole program, and it is a compile error, never wrong code. */
static uint64_t str_hash(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ULL;
    }
    return h;
}

static int hash_is(uint64_t h, const char *s)
{
    return h == str_hash(s);
}

/* Per-function scratch text.  Identifier *references* are hashes, but two
 * things still need the bytes: N_DECL (declare_locals and gen_stmt bind the
 * symbol by name) and struct field paths ("in.y" cannot be split from a hash).
 * Both are parsed into this pool and dropped when the function is done. */
#define TEXT_POOL 3072
static char text_pool[TEXT_POOL];
static int text_used;

static uint32_t text_add(const char *name)
{
    int n = (int)strlen(name) + 1;
    uint32_t off;
    if (text_used + n > TEXT_POOL)
        die("function text pool full");
    off = (uint32_t)text_used;
    memcpy(text_pool + text_used, name, (size_t)n);
    text_used += n;
    return off;
}

static const char *text_at(uint32_t off)
{
    return text_pool + off;
}

static Node *node(int kind)
{
    if (nnode >= NODE_MAX) die("AST full");
    Node *n = &nodes[nnode++];
    memset(n, 0, sizeof *n);
    n->kind = kind;
    n->line = src_line;
    return n;
}

static Sym *sym_find(const char *name)
{
    /* Search newest first so locals shadow globals. */
    for (int i = nsyms - 1; i >= 0; i--)
        if (!strcmp(syms[i].name, name)) return &syms[i];
    return NULL;
}

static Sym *sym_find_hash(uint64_t h)
{
    for (int i = nsyms - 1; i >= 0; i--)
        if (syms[i].hash == h) return &syms[i];
    return NULL;
}

static Sym *sym_add(const char *name, int global)
{
    uint64_t h;
    int i;
    if (nsyms >= SYM_MAX) die("symbol table full");
    h = str_hash(name);
    for (i = 0; i < nsyms; i++) {
        if (syms[i].hash == h && strcmp(syms[i].name, name))
            die("identifier hash collision");
        /* No block scope yet (issue #3): a second local of the same name
         * would silently capture every earlier reference.  Reject it
         * instead of computing with the wrong variable. */
        if (!global && !syms[i].global && syms[i].hash == h &&
            !strcmp(syms[i].name, name))
            die("duplicate local variable (block scope not supported)");
    }
    Sym *s = &syms[nsyms++];
    memset(s, 0, sizeof *s);
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->hash = h;
    s->global = global;
    s->reg = global ? -1 : nlocals++;
    return s;
}

static Sym *sym_add_global(const char *name)
{
    Sym *s = sym_add(name, 1);
    if (gdata_next >= GMAX)
        die("too many global words (max 512)");
    s->offset = gdata_next++;
    return s;
}

static Sym *sym_add_array(const char *name, int len, int is_global)
{
    Sym *s = sym_add(name, is_global);
    s->is_array = 1;
    s->alen = len;
    s->offset = gdata_next;
    gdata_next += len;
    return s;
}

static int struct_lookup(const char *name)
{
    for (int i = 0; i < nstructs; i++)
        if (!strcmp(structs[i].name, name)) return i + 1;
    return 0;
}

static int struct_field(int stype, const char *name)
{
    if (stype <= 0 || stype > nstructs) return -1;
    stype--;
    for (int i = 0; i < structs[stype].nfields; i++)
        if (!strcmp(structs[stype].fields[i], name)) return i;
    return -1;
}

/* Resolve a dotted field path such as "in.y".  Returns the final field type
 * (0 int, >0 struct) and accumulates the word offset. */
static int struct_path(int sid, const char *path, int *off)
{
    int cur = sid, total = 0;
    const char *p = path;

    if (cur <= 0)
        return -1;
    while (*p) {
        char seg[32];
        int n = 0, fi;

        while (*p && *p != '.') {
            if (n < (int)sizeof(seg) - 1)
                seg[n++] = *p;
            p++;
        }
        seg[n] = 0;
        if (*p == '.')
            p++;
        fi = struct_field(cur, seg);
        if (fi < 0)
            die("unknown struct field");
        total += structs[cur - 1].foff[fi];
        cur = structs[cur - 1].ftype[fi];
        if (*p && cur <= 0)
            die("field is not a struct");
    }
    *off = total;
    return cur;
}

static Sym *sym_add_struct(const char *name, int stype, int global)
{
    Sym *s = sym_add(name, global);
    s->is_struct = 1;
    s->stype = stype;
    s->offset = gdata_next;
    gdata_next += structs[stype - 1].size;
    if (!global) {
        s->reg = -1;
        nlocals--;
    }
    return s;
}

static Sym *sym_add_struct_array(const char *name, int stype, int len, int global)
{
    Sym *s = sym_add(name, global);
    s->is_struct = 1;
    s->is_array = 1;
    s->stype = stype;
    s->alen = len;
    s->offset = gdata_next;
    gdata_next += structs[stype - 1].size * len;
    if (!global) {
        s->reg = -1;
        nlocals--;
    }
    return s;
}

static Sym *sym_add_float(const char *name, int global)
{
    Sym *s = sym_add(name, global);
    s->is_float = 1;
    if (global)
        s->offset = gdata_next++;
    return s;
}

static int arena_reserve(int len)
{
    int off = arena_next;

    if (len < 0 || arena_next + len > ARENA_MAX)
        die("string data too large");
    memset(arena + arena_next, 0, (size_t)len);
    arena_next += len;
    return off;
}

static Sym *sym_add_char(const char *name, int global)
{
    Sym *s = sym_add(name, global);
    s->is_char = 1;
    if (global)
        s->offset = gdata_next++;
    return s;
}

static Sym *sym_add_string(const char *name, int global)
{
    Sym *s = sym_add(name, global);
    s->is_string = 1;
    if (global)
        s->offset = gdata_next++;
    return s;
}

static Sym *sym_add_char_array(const char *name, int len, int global)
{
    Sym *s = sym_add(name, global);
    s->is_char_array = 1;
    s->alen = len;
    s->offset = arena_reserve(len);
    if (!global) {
        s->reg = -1;      /* no word slot; the arena holds the bytes */
        nlocals--;
    }
    return s;
}

/* Copy a literal into a reserved arena region, always NUL-terminated. */
static void arena_set_literal(int off, int cap, int spoff)
{
    int len = (int)strlen(strpool + spoff);
    int n = len < cap - 1 ? len : cap - 1;

    if (cap <= 0)
        return;
    memcpy(arena + off, strpool + spoff, (size_t)n);
    arena[off + n] = 0;
}

static int arena_literal(int spoff)
{
    int len = (int)strlen(strpool + spoff) + 1;
    int off = arena_reserve(len);

    memcpy(arena + off, strpool + spoff, (size_t)len);
    return off;
}

static int const_lookup(const char *name, int *out)
{
    for (int i = nconsts - 1; i >= 0; i--)
        if (!strcmp(consts[i].name, name)) {
            *out = consts[i].val;
            return 1;
        }
    return 0;
}

static int const_lookup_hash(uint64_t h, int *out)
{
    for (int i = nconsts - 1; i >= 0; i--)
        if (str_hash(consts[i].name) == h) {
            *out = consts[i].val;
            return 1;
        }
    return 0;
}

static void const_add(const char *name, int val)
{
    if (nconsts >= CONST_MAX) die("too many enum constants");
    memset(&consts[nconsts], 0, sizeof consts[nconsts]);
    strncpy(consts[nconsts].name, name, sizeof(consts[nconsts].name) - 1);
    consts[nconsts].val = val;
    nconsts++;
}

static int new_temp(void)
{
    if (vreg >= VREG_MAX) die("out of virtual registers");
    return vreg++;
}

static Node *parse_expr(void);

static void read_field_path(char *out, int cap)
{
    int n = 0;

    for (;;) {
        int i;
        if (tok_kind != 1)
            die("field name expected");
        if (n > 0 && n < cap - 1)
            out[n++] = '.';
        for (i = 0; tok[i] && n < cap - 1; i++)
            out[n++] = tok[i];
        next();
        if (!(tok_kind == 3 && !strcmp(tok, ".")))
            break;
        next();
    }
    out[n] = 0;
}

static Node *parse_primary(void)
{
    if (tok_kind == 2) {
        Node *n = node(N_NUM); n->val = tok_num; next(); return n;
    }
    if (tok_kind == 5) {
        Node *n = node(N_NUM); n->val = tok_num; n->ftype = 1; next(); return n;
    }
    if (tok_kind == 4) {
        Node *n = node(N_STR);
        n->val = tok_num;
        n->alen = (int)strlen(strpool + tok_num);
        next();
        return n;
    }
    if (tok_kind == 1) {
        char name[32];
        strncpy(name, tok, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        next();
        if (accept("(")) {
            Node *n = node(N_CALL);
            n->hash = str_hash(name);
            if (!accept(")")) {
                Node **p = &n->args;
                for (;;) {
                    *p = parse_expr();
                    p = &(*p)->next;
                    if (accept(")")) break;
                    expect(",");
                }
            }
            return n;
        }
        if (accept("[")) {
            Node *idx = parse_expr();
            expect("]");
            if (accept(".")) {
                char member[32];
                Node *n;
                read_field_path(member, sizeof member);
                n = node(N_MEMBER_INDEX);
                n->hash = str_hash(name);
                n->field_off = text_add(member);
                n->lhs = idx;
                return n;
            }
            {
                Node *n = node(N_INDEX);
                n->hash = str_hash(name);
                n->lhs = idx;
                return n;
            }
        }
        if (accept(".")) {
            char member[32];
            read_field_path(member, sizeof member);
            if (accept("(")) {
                if (strchr(member, '.'))
                    die("method name cannot be dotted");
                Node *call = node(N_CALL);
                Node *base = node(N_VAR);
                Node **p = &base->next;
                call->hash = str_hash(member);
                base->hash = str_hash(name);
                call->args = base;
                if (!accept(")")) {
                    for (;;) {
                        *p = parse_expr();
                        p = &(*p)->next;
                        if (accept(")")) break;
                        expect(",");
                    }
                }
                return call;
            }
            Node *n = node(N_MEMBER);
            n->hash = str_hash(name);
            n->field_off = text_add(member);
            return n;
        }
        Node *n = node(N_VAR);
        n->hash = str_hash(name);
        return n;
    }
    if (accept("(")) {
        Node *n = parse_expr();
        expect(")");
        return n;
    }
    die("expression expected");
    return NULL;
}

static Node *parse_unary(void)
{
    if (accept("-")) {
        Node *z = node(N_NUM);
        Node *x = node(N_SUB);
        z->val = 0;
        x->lhs = z;
        x->rhs = parse_unary();
        return x;
    }
    if (accept("+"))
        return parse_unary();
    if (accept("!")) {
        Node *z = node(N_NUM);
        Node *x = node(N_EQ);
        z->val = 0;
        x->lhs = parse_unary();
        x->rhs = z;
        return x;
    }
    return parse_primary();
}

static Node *parse_mul(void)
{
    Node *n = parse_unary();
    for (;;) {
        if (accept("*")) { Node *x = node(N_MUL); x->lhs = n; x->rhs = parse_unary(); n = x; }
        else if (accept("/")) { Node *x = node(N_DIV); x->lhs = n; x->rhs = parse_unary(); n = x; }
        else if (accept("%")) { Node *x = node(N_MOD); x->lhs = n; x->rhs = parse_unary(); n = x; }
        else break;
    }
    return n;
}

static Node *parse_add(void)
{
    Node *n = parse_mul();
    for (;;) {
        if (accept("+")) { Node *x = node(N_ADD); x->lhs = n; x->rhs = parse_mul(); n = x; }
        else if (accept("-")) { Node *x = node(N_SUB); x->lhs = n; x->rhs = parse_mul(); n = x; }
        else break;
    }
    return n;
}

static Node *parse_rel(void)
{
    Node *n = parse_add();
    for (;;) {
        if (accept("<=")) { Node *x = node(N_LE); x->lhs = n; x->rhs = parse_add(); n = x; }
        else if (accept(">=")) { Node *x = node(N_GE); x->lhs = n; x->rhs = parse_add(); n = x; }
        else if (accept("!=")) { Node *x = node(N_NE); x->lhs = n; x->rhs = parse_add(); n = x; }
        else if (accept("<")) { Node *x = node(N_LT); x->lhs = n; x->rhs = parse_add(); n = x; }
        else if (accept(">")) { Node *x = node(N_GT); x->lhs = n; x->rhs = parse_add(); n = x; }
        else if (accept("==")) { Node *x = node(N_EQ); x->lhs = n; x->rhs = parse_add(); n = x; }
        else break;
    }
    return n;
}

static Node *parse_land(void)
{
    Node *n = parse_rel();
    while (accept("&&")) {
        Node *x = node(N_AND);
        x->lhs = n;
        x->rhs = parse_rel();
        n = x;
    }
    return n;
}

static Node *parse_lor(void)
{
    Node *n = parse_land();
    while (accept("||")) {
        Node *x = node(N_OR);
        x->lhs = n;
        x->rhs = parse_land();
        n = x;
    }
    return n;
}

static int to_fix(int v)
{
    return (int)((uint32_t)v << 16);
}

/* Same Q16.16 arithmetic as rvm's fmul16/fdiv16, so compile-time folding
 * and runtime results agree bit for bit. */
static int fix_mul(int a, int b)
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

static int fix_div(int a, int b)
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

static Node *fold_const(Node *n)
{
    if (!n)
        return n;
    n->lhs = fold_const(n->lhs);
    n->rhs = fold_const(n->rhs);
    if (n->lhs && n->rhs &&
        n->lhs->kind == N_NUM && n->rhs->kind == N_NUM &&
        (n->kind == N_ADD || n->kind == N_SUB || n->kind == N_MUL ||
         n->kind == N_DIV || n->kind == N_MOD ||
         n->kind == N_LT || n->kind == N_GT || n->kind == N_EQ ||
         n->kind == N_NE || n->kind == N_LE || n->kind == N_GE ||
         n->kind == N_AND || n->kind == N_OR)) {
        int isf = n->lhs->ftype || n->rhs->ftype;
        int a = n->lhs->val, b = n->rhs->val, v = 0, rfloat = 0;

        if (isf) {
            if (!n->lhs->ftype) a = to_fix(a);
            if (!n->rhs->ftype) b = to_fix(b);
        }
        if (n->kind == N_ADD) { v = a + b; rfloat = isf; }
        else if (n->kind == N_SUB) { v = a - b; rfloat = isf; }
        else if (n->kind == N_MUL) { v = isf ? fix_mul(a, b) : a * b; rfloat = isf; }
        else if (n->kind == N_DIV) {
            /* Divide by zero is defined as 0 in rvm/rcc; keep the node so
             * the backend applies the same rule (issue #2).  INT_MIN / -1
             * overflows in C, so fold it to the same saturated result the
             * RV32/ARM divide instructions produce. */
            if (b == 0)
                return n;
            if (!isf && a == INT32_MIN && b == -1)
                v = INT32_MIN;
            else
                v = isf ? fix_div(a, b) : a / b;
            rfloat = isf;
        } else if (n->kind == N_MOD) {
            if (isf || b == 0)
                return n;
            /* INT_MIN % -1 overflows in C; the native backends define 0. */
            if (a == INT32_MIN && b == -1)
                v = 0;
            else
                v = a % b;
        } else if (n->kind == N_LT) v = a < b;
        else if (n->kind == N_GT) v = a > b;
        else if (n->kind == N_EQ) v = a == b;
        else if (n->kind == N_NE) v = a != b;
        else if (n->kind == N_LE) v = a <= b;
        else if (n->kind == N_GE) v = a >= b;
        else if (n->kind == N_AND) v = (a != 0) && (b != 0);
        else v = (a != 0) || (b != 0);
        n->kind = N_NUM;
        n->val = v;
        n->ftype = rfloat;
        n->lhs = n->rhs = NULL;
    }
    return n;
}

static Node *parse_expr(void)
{
    return fold_const(parse_lor());
}

static int parse_const_expr(void)
{
    int sign = 1;
    int v = 0;

    if (accept("-"))
        sign = -1;
    if (tok_kind == 2) {
        v = tok_num;
        next();
    } else if (tok_kind == 1) {
        if (!const_lookup(tok, &v))
            die("constant expected");
        next();
    } else {
        die("constant expected");
    }
    return sign * v;
}

/* ---- lvalue helpers for compound assignment and ++/-- ------------------ */

static Node *mk_one(void)
{
    Node *n = node(N_NUM);
    n->val = 1;
    return n;
}

static Node *mk_bin(int kind, Node *a, Node *b)
{
    Node *n = node(kind);
    n->lhs = a;
    n->rhs = b;
    return n;
}

/* Build the assignment that writes `value` back to the same place as `load`. */
static Node *assign_from_load(Node *load, Node *value)
{
    Node *n;

    switch (load->kind) {
    case N_VAR:
        n = node(N_ASSIGN);
        n->hash = load->hash;
        n->lhs = value;
        break;
    case N_MEMBER:
        n = node(N_ASSIGN_MEMBER);
        n->hash = load->hash;
        n->field_off = load->field_off;
        n->lhs = value;
        break;
    case N_INDEX:
        n = node(N_ASSIGN_INDEX);
        n->hash = load->hash;
        n->lhs = load->lhs;
        n->rhs = value;
        break;
    case N_MEMBER_INDEX:
        n = node(N_ASSIGN_MEMBER_INDEX);
        n->hash = load->hash;
        n->field_off = load->field_off;
        n->lhs = load->lhs;
        n->rhs = value;
        break;
    default:
        die_node(load, "invalid assignment target");
    }
    return n;
}

static int compound_kind(void)
{
    if (tok_kind != 3)
        return 0;
    if (!strcmp(tok, "+=")) return N_ADD;
    if (!strcmp(tok, "-=")) return N_SUB;
    if (!strcmp(tok, "*=")) return N_MUL;
    if (!strcmp(tok, "/=")) return N_DIV;
    if (!strcmp(tok, "%=")) return N_MOD;
    return 0;
}

/* Parse an optional postfix ++/-- or compound operator after `load`.  The
 * caller has already parsed the place.  Returns NULL when the token is a
 * plain '=' (or another statement form). */
static Node *parse_place_tail(Node *load, int semi)
{
    int ck = compound_kind();

    if (ck) {
        next();
        return assign_from_load(load, mk_bin(ck, load, parse_expr()));
    }
    if (accept("++"))
        return assign_from_load(load, mk_bin(N_ADD, load, mk_one()));
    if (accept("--"))
        return assign_from_load(load, mk_bin(N_SUB, load, mk_one()));
    (void)semi;
    return NULL;
}

/* Declaration or assignment, optionally consuming the trailing semicolon.
 * Used by normal statements and by for(init; cond; step). */
static Node *parse_simple(int semi)
{
    if (tok_kind == 3 && (!strcmp(tok, "++") || !strcmp(tok, "--"))) {
        int inc = tok[0] == '+';
        char name[32];
        Node *load, *val, *as;
        next();
        if (tok_kind != 1) die("variable expected");
        strncpy(name, tok, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        next();
        if (accept(".")) {
            char member[32];
            read_field_path(member, sizeof member);
            load = node(N_MEMBER);
            load->hash = str_hash(name);
            load->field_off = text_add(member);
        } else if (accept("[")) {
            Node *idx = parse_expr();
            expect("]");
            if (accept(".")) {
                char member[32];
                read_field_path(member, sizeof member);
                load = node(N_MEMBER_INDEX);
                load->hash = str_hash(name);
                load->field_off = text_add(member);
                load->lhs = idx;
            } else {
                load = node(N_INDEX);
                load->hash = str_hash(name);
                load->lhs = idx;
            }
        } else {
            load = node(N_VAR);
            load->hash = str_hash(name);
        }
        val = mk_bin(inc ? N_ADD : N_SUB, load, mk_one());
        as = assign_from_load(load, val);
        if (semi) expect(";");
        return as;
    }
    if (is_kw("char")) {
        Node *n;
        next();
        if (tok_kind != 1) die("name expected");
        n = node(N_DECL);
        n->ctype = 1;
        n->hash = text_add(tok);
        next();
        if (accept("[")) {
            if (tok_kind != 2) die("array length expected");
            n->alen = tok_num;
            n->ctype = 2;
            next();
            expect("]");
        }
        if (accept("=")) {
            if (n->ctype == 2) {
                if (tok_kind != 4) die("string initializer expected");
                {
                    Node *lit = node(N_STR);
                    lit->val = tok_num;
                    lit->alen = (int)strlen(strpool + tok_num);
                    next();
                    n->lhs = lit;
                }
            } else {
                n->lhs = parse_expr();
            }
        }
        if (semi) expect(";");
        return n;
    }
    if (is_kw("string")) {
        Node *n;
        next();
        if (tok_kind != 1) die("name expected");
        n = node(N_DECL);
        n->ctype = 3;
        n->hash = text_add(tok);
        next();
        if (accept("="))
            n->lhs = parse_expr();
        if (semi) expect(";");
        return n;
    }
    if (is_kw("struct")) {
        int sid;
        Node *n;
        next();
        if (tok_kind != 1) die("struct type expected");
        sid = struct_lookup(tok);
        if (sid <= 0) die("unknown struct type");
        next();
        if (tok_kind != 1) die("variable name expected");
        n = node(N_DECL);
        n->stype = sid;
        n->hash = text_add(tok);
        next();
        if (accept("[")) {
            if (tok_kind != 2) die("struct array length expected");
            n->alen = tok_num;
            next();
            expect("]");
        }
        if (semi) expect(";");
        return n;
    }
    if (is_kw("float")) {
        Node *n;
        next();
        if (tok_kind != 1) die("name expected");
        n = node(N_DECL);
        n->ftype = 1;
        n->hash = text_add(tok);
        next();
        if (accept("="))
            n->lhs = parse_expr();
        if (semi) expect(";");
        return n;
    }
    if (is_kw("int")) {
        Node *n;
        next();
        if (tok_kind != 1) die("name expected");
        n = node(N_DECL);
        n->hash = text_add(tok);
        next();
        if (accept("[")) {
            if (tok_kind != 2) die("array length expected");
            n->alen = tok_num;
            next();
            expect("]");
        }
        if (accept("=")) {
            if (n->alen) die("array initializer unsupported");
            n->lhs = parse_expr();
        }
        if (semi) expect(";");
        return n;
    }
    if (tok_kind == 1) {
        char name[32];
        strncpy(name, tok, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        next();
        if (accept(".")) {
            char member[32];
            read_field_path(member, sizeof member);
            if (accept("(")) {
                if (strchr(member, '.'))
                    die("method name cannot be dotted");
                Node *call = node(N_CALL);
                Node *base = node(N_VAR);
                Node **p = &base->next;
                call->hash = str_hash(member);
                base->hash = str_hash(name);
                call->args = base;
                if (!accept(")")) {
                    for (;;) {
                        *p = parse_expr();
                        p = &(*p)->next;
                        if (accept(")")) break;
                        expect(",");
                    }
                }
                if (semi) expect(";");
                return call;
            }
            {
                Node *load = node(N_MEMBER);
                Node *tail;
                load->hash = str_hash(name);
                load->field_off = text_add(member);
                tail = parse_place_tail(load, semi);
                if (tail) {
                    if (semi) expect(";");
                    return tail;
                }
            }
            expect("=");
            {
                Node *n = node(N_ASSIGN_MEMBER);
                n->hash = str_hash(name);
                n->field_off = text_add(member);
                n->lhs = parse_expr();
                if (semi) expect(";");
                return n;
            }
        }
        if (accept("(")) {
            Node *n = node(N_CALL);
            n->hash = str_hash(name);
            if (!accept(")")) {
                Node **p = &n->args;
                for (;;) {
                    *p = parse_expr();
                    p = &(*p)->next;
                    if (accept(")")) break;
                    expect(",");
                }
            }
            if (semi) expect(";");
            return n;
        }
        if (accept("[")) {
            Node *idx = parse_expr();
            expect("]");
            if (accept(".")) {
                char member[32];
                Node *n;
                read_field_path(member, sizeof member);
                {
                    Node *load = node(N_MEMBER_INDEX);
                    Node *tail;
                    load->hash = str_hash(name);
                    load->field_off = text_add(member);
                    load->lhs = idx;
                    tail = parse_place_tail(load, semi);
                    if (tail) {
                        if (semi) expect(";");
                        return tail;
                    }
                }
                expect("=");
                n = node(N_ASSIGN_MEMBER_INDEX);
                n->hash = str_hash(name);
                n->field_off = text_add(member);
                n->lhs = idx;
                n->rhs = parse_expr();
                if (semi) expect(";");
                return n;
            }
            {
                Node *load = node(N_INDEX);
                Node *tail;
                load->hash = str_hash(name);
                load->lhs = idx;
                tail = parse_place_tail(load, semi);
                if (tail) {
                    if (semi) expect(";");
                    return tail;
                }
            }
            {
                Node *n = node(N_ASSIGN_INDEX);
                n->hash = str_hash(name);
                n->lhs = idx;
                expect("=");
                n->rhs = parse_expr();
                if (semi) expect(";");
                return n;
            }
        }
        {
            Node *n = node(N_ASSIGN);
            Node *v = node(N_VAR);
            n->hash = str_hash(name);
            v->hash = str_hash(name);
            if (tok_kind == 3 &&
                (!strcmp(tok, "+=") || !strcmp(tok, "-=") ||
                 !strcmp(tok, "*=") || !strcmp(tok, "/=") ||
                 !strcmp(tok, "%="))) {
                char op = tok[0];
                Node *x;
                next();
                if (op == '+') x = node(N_ADD);
                else if (op == '-') x = node(N_SUB);
                else if (op == '*') x = node(N_MUL);
                else if (op == '/') x = node(N_DIV);
                else x = node(N_MOD);
                x->lhs = v;
                x->rhs = parse_expr();
                n->lhs = x;
            } else if (accept("++")) {
                Node *x = node(N_ADD);
                Node *one = node(N_NUM);
                one->val = 1;
                x->lhs = v;
                x->rhs = one;
                n->lhs = x;
            } else if (accept("--")) {
                Node *x = node(N_SUB);
                Node *one = node(N_NUM);
                one->val = 1;
                x->lhs = v;
                x->rhs = one;
                n->lhs = x;
            } else {
                expect("=");
                n->lhs = parse_expr();
            }
            if (semi) expect(";");
            return n;
        }
    }
    die("statement expected");
    return NULL;
}

static Node *parse_stmt(void)
{
    if (accept("{")) {
        Node *b = node(N_BLOCK);
        Node **p = &b->body;
        while (!accept("}")) {
            if (tok_kind == 0) die("unterminated block");
            *p = parse_stmt();
            p = &(*p)->next;
        }
        return b;
    }
    if (is_kw("print")) {
        next(); expect("(");
        Node *n = node(N_PRINT);
        n->lhs = parse_expr();
        expect(")");
        expect(";");
        return n;
    }
    if (is_kw("printf")) {
        next(); expect("(");
        if (tok_kind != 4) die("printf format string expected");
        Node *n = node(N_PRINTF);
        n->val = tok_num;
        n->alen = (int)strlen(strpool + tok_num);
        next();
        if (accept(","))
            n->args = parse_expr();
        expect(")");
        expect(";");
        return n;
    }
    if (is_kw("puts")) {
        next(); expect("(");
        if (tok_kind != 4) die("string expected");
        Node *n = node(N_PUTS);
        n->val = tok_num;
        n->alen = (int)strlen(strpool + tok_num);
        next();
        expect(")");
        expect(";");
        return n;
    }
    if (is_kw("return")) {
        next();
        Node *n = node(N_RETURN);
        n->lhs = parse_expr();
        expect(";");
        return n;
    }
    if (is_kw("if")) {
        next(); expect("(");
        Node *n = node(N_IF);
        n->cond = parse_expr(); expect(")");
        n->then = parse_stmt();
        if (is_kw("else")) { next(); n->els = parse_stmt(); }
        return n;
    }
    if (is_kw("while")) {
        next(); expect("(");
        Node *n = node(N_WHILE);
        n->cond = parse_expr(); expect(")");
        n->body = parse_stmt();
        return n;
    }
    if (is_kw("break")) {
        next(); expect(";");
        return node(N_BREAK);
    }
    if (is_kw("continue")) {
        next(); expect(";");
        return node(N_CONTINUE);
    }
    if (is_kw("switch")) {
        Node *n = node(N_SWITCH);
        Node **tail;
        next(); expect("(");
        n->lhs = parse_expr();
        expect(")");
        expect("{");
        tail = &n->body;
        while (!accept("}")) {
            Node *c;
            int isdef = 0, cval = 0;
            Node **st;
            if (is_kw("case")) {
                next();
                cval = parse_const_expr();
                expect(":");
            } else if (is_kw("default")) {
                next();
                expect(":");
                isdef = 1;
            } else {
                die("case/default expected");
            }
            c = node(N_CASE);
            c->val = cval;
            c->stype = isdef;
            st = &c->body;
            while (!is_kw("case") && !is_kw("default") &&
                   !(tok_kind == 3 && !strcmp(tok, "}"))) {
                if (tok_kind == 0) die("unterminated switch");
                *st = parse_stmt();
                st = &(*st)->next;
            }
            *tail = c;
            tail = &c->next;
        }
        return n;
    }
    if (is_kw("do")) {
        Node *n = node(N_DO);
        next();
        n->body = parse_stmt();
        if (!is_kw("while")) die("while expected after do");
        next(); expect("(");
        n->cond = parse_expr();
        expect(")");
        expect(";");
        return n;
    }
    if (is_kw("for")) {
        Node *n = node(N_FOR);
        next(); expect("(");
        if (accept(";")) n->lhs = NULL;
        else { n->lhs = parse_simple(0); expect(";"); }
        if (accept(";")) n->cond = NULL;
        else { n->cond = parse_expr(); expect(";"); }
        if (accept(")")) n->rhs = NULL;
        else { n->rhs = parse_simple(0); expect(")"); }
        n->body = parse_stmt();
        return n;
    }
    return parse_simple(1);
}

static int emit(int op, int a, int b, int c, int d)
{
    if (nir >= IR_MAX) die("RIR full");
    ir[nir] = (RIR){ op, a, b, c, d };
    return nir++;
}

static int func_lookup(const char *name)
{
    for (int i = 0; i < nfuncs; i++)
        if (!strcmp(sig_name(i), name)) return i;
    return -1;
}

static int func_lookup_hash(uint64_t h)
{
    for (int i = 0; i < nfuncs; i++)
        if (str_hash(sig_name(i)) == h) return i;
    return -1;
}

/* Native ids shared with rvm. */
enum {
    NAT_GPIO_COUNT, NAT_GPIO_MODE, NAT_GPIO_WRITE, NAT_GPIO_READ,
    NAT_ADC_COUNT, NAT_ADC_READ, NAT_PWM_COUNT, NAT_PWM_SET, NAT_PWM_OFF,
    NAT_SLEEP, NAT_UPTIME, NAT_RUN,
    NAT_OPEN, NAT_CREATE, NAT_READ, NAT_WRITE, NAT_CLOSE, NAT_UNLINK,
    NAT_CAPTURE, NAT_READDIR, NAT_ISDIR
};


static int native_lookup_hash(uint64_t h, int *nargs, int *hasret)
{
    static const struct { const char *name; int id, nargs, hasret; } tab[] = {
        { "gpio_count", NAT_GPIO_COUNT, 0, 1 },
        { "gpio_mode",  NAT_GPIO_MODE,  2, 0 },
        { "gpio_write", NAT_GPIO_WRITE, 2, 0 },
        { "gpio_read",  NAT_GPIO_READ,  1, 1 },
        { "adc_count",  NAT_ADC_COUNT,  0, 1 },
        { "adc_read",   NAT_ADC_READ,   1, 1 },
        { "pwm_count",  NAT_PWM_COUNT,  0, 1 },
        { "pwm_set",    NAT_PWM_SET,    3, 0 },
        { "pwm_off",    NAT_PWM_OFF,    1, 0 },
        { "sleep",      NAT_SLEEP,      1, 0 },
        { "uptime",     NAT_UPTIME,     0, 1 },
        { "run",        NAT_RUN,        1, 1 },
        { "open",       NAT_OPEN,       1, 1 },
        { "create",     NAT_CREATE,     1, 1 },
        { "read",       NAT_READ,       3, 1 },
        { "write",      NAT_WRITE,      3, 1 },
        { "close",      NAT_CLOSE,      1, 1 },
        { "unlink",     NAT_UNLINK,     1, 1 },
        { "capture",    NAT_CAPTURE,    3, 1 },
        { "readdir",    NAT_READDIR,    3, 1 },
        { "isdir",      NAT_ISDIR,      1, 1 },
    };
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (str_hash(tab[i].name) == h) {
            *nargs = tab[i].nargs;
            *hasret = tab[i].hasret;
            return tab[i].id;
        }
    return -1;
}

/* Reserve registers for every local declaration in the function body. */
static void declare_locals(Node *n)
{
    for (; n; n = n->next) {
        /* Diagnostics from sym_add* should name the declaration, not the
         * end of the function after the whole body was parsed. */
        int saved_line = src_line;

        src_line = n->line;
        switch (n->kind) {
        case N_BLOCK:
            declare_locals(n->body);
            break;
        case N_DECL:
            if (n->ctype == 2) sym_add_char_array(text_at((uint32_t)n->hash), n->alen, 0);
            else if (n->ctype == 3) sym_add_string(text_at((uint32_t)n->hash), 0);
            else if (n->ctype == 1) sym_add_char(text_at((uint32_t)n->hash), 0);
            else if (n->stype > 0) {
                if (n->alen > 0) sym_add_struct_array(text_at((uint32_t)n->hash), n->stype, n->alen, 0);
                else sym_add_struct(text_at((uint32_t)n->hash), n->stype, 0);
            }
            else if (n->ftype) sym_add_float(text_at((uint32_t)n->hash), 0);
            else if (n->alen > 0) sym_add_array(text_at((uint32_t)n->hash), n->alen, 0);
            else sym_add(text_at((uint32_t)n->hash), 0);
            break;
        case N_IF:
            declare_locals(n->then);
            declare_locals(n->els);
            break;
        case N_WHILE:
            declare_locals(n->body);
            break;
        case N_DO:
            declare_locals(n->body);
            break;
        case N_SWITCH:
            for (Node *c = n->body; c; c = c->next)
                declare_locals(c->body);
            break;
        case N_FOR:
            declare_locals(n->lhs);
            declare_locals(n->rhs);
            declare_locals(n->body);
            break;
        default:
            break;
        }
        src_line = saved_line;
    }
}

static int gen_expr_into(Node *n, int dst);
static int gen_expr(Node *n);

/* A value that can be used in place, without copying a plain local. */
static int node_float_hint(Node *n)
{
    if (!n) return 0;
    if (n->kind == N_NUM) return n->ftype ? 1 : 0;
    if (n->kind == N_STR) return -1;
    if (n->kind == N_VAR) {
        Sym *s = sym_find_hash(n->hash);
        if (s && (s->is_string || s->is_char_array)) return -1;
        return (s && s->is_float) ? 1 : 0;
    }
    if (n->kind == N_INDEX || n->kind == N_MEMBER) return 0;
    if (n->kind == N_AND || n->kind == N_OR) return 0;
    if (n->kind == N_ADD || n->kind == N_SUB || n->kind == N_MUL ||
        n->kind == N_DIV || n->kind == N_MOD ||
        n->kind == N_LT || n->kind == N_GT || n->kind == N_EQ ||
        n->kind == N_NE || n->kind == N_LE || n->kind == N_GE) {
        int l = node_float_hint(n->lhs), r = node_float_hint(n->rhs);
        if (l == 1 || r == 1) return 1;
        if (l == 0 && r == 0) return 0;
    }
    return -1;   /* composite: the result type is known after codegen */
}

static int coerce_to(int v, int from, int to)
{
    int d;
    if (from == 2 || to == 2)
        die("string used as a number");
    if (from == to) return v;
    d = new_temp();
    emit(to ? R_SHLI : R_SHRI, v, 16, 0, d);
    return d;
}

static int gen_value(Node *n)
{
    if (n->kind == N_VAR) {
        Sym *s = sym_find_hash(n->hash);
        if (!s) {
            int v;
            if (const_lookup_hash(n->hash, &v)) {
                int d = new_temp();
                emit(R_CONST, v, 0, 0, d);
                last_type = 0;
                return d;
            }
            die_node(n, "unknown variable");
        }
        if (s->is_char_array) {
            int d = new_temp();
            emit(R_CONST, s->offset, 0, 0, d);
            last_type = 2;
            return d;
        }
        if (s->is_string) {
            int d;
            if (!s->global) {
                last_type = 2;
                return s->reg;
            }
            d = new_temp();
            emit(R_LOADG, s->offset, 0, 0, d);
            last_type = 2;
            return d;
        }
        if (s->is_char) {
            int d;
            if (!s->global) {
                last_type = 0;
                return s->reg;
            }
            d = new_temp();
            emit(R_LOADG, s->offset, 0, 0, d);
            last_type = 0;
            return d;
        }
        if (s->is_array) die_node(n, "array used as value");
        if (s->is_struct) {
            int d;
            last_type = 0;
            if (s->is_param) return s->reg;
            d = new_temp();
            emit(R_CONST, s->offset, 0, 0, d);
            return d;
        }
        if (!s->global) {
            last_type = s->is_float;
            return s->reg;
        }
    }
    return gen_expr(n);
}

/* Base register of a struct object, or of one element when idx >= 0. */
static int struct_base(Sym *s, int idx)
{
    if (idx < 0) {
        int d;
        if (s->is_param) return s->reg;
        d = new_temp();
        emit(R_CONST, s->offset, 0, 0, d);
        return d;
    }
    {
        int nf = structs[s->stype - 1].size;
        int t = new_temp();
        emit(R_MULI, idx, nf, 0, t);
        emit(R_ADDI, t, s->offset, 0, t);
        return t;
    }
}

static int node_struct_type(Node *n)
{
    if (!n) return 0;
    if (n->kind == N_VAR) {
        Sym *s = sym_find_hash(n->hash);
        return (s && s->is_struct) ? s->stype : 0;
    }
    if (n->kind == N_MEMBER || n->kind == N_MEMBER_INDEX) {
        Sym *s = sym_find_hash(n->hash);
        int off;
        if (!s || !s->is_struct) return 0;
        return struct_path(s->stype, text_at(n->field_off), &off);
    }
    if (n->kind == N_CALL) {
        int fid = func_lookup_hash(n->hash);
        return fid >= 0 ? sigs[fid].rtype_struct : 0;
    }
    return 0;
}

/* Emit a register holding the word base of a struct-valued expression. */
static int gen_struct_ref(Node *n)
{
    Sym *s;
    int poff, ptype;

    if (n->kind == N_VAR) {
        s = sym_find_hash(n->hash);
        if (!s || !s->is_struct) die_node(n, "struct value expected");
        if (s->is_param) return s->reg;
        {
            int d = new_temp();
            emit(R_CONST, s->offset, 0, 0, d);
            return d;
        }
    }
    if (n->kind == N_MEMBER) {
        s = sym_find_hash(n->hash);
        if (!s || !s->is_struct) die_node(n, "struct value expected");
        ptype = struct_path(s->stype, text_at(n->field_off), &poff);
        if (ptype <= 0) die_node(n, "field is not a struct");
        {
            int base = s->is_param ? s->reg : new_temp();
            if (!s->is_param) emit(R_CONST, s->offset, 0, 0, base);
            if (poff) {
                int t = new_temp();
                emit(R_ADDI, base, poff, 0, t);
                return t;
            }
            return base;
        }
    }
    if (n->kind == N_MEMBER_INDEX) {
        int idx, base;
        s = sym_find_hash(n->hash);
        if (!s || !s->is_struct || !s->is_array) die_node(n, "struct array expected");
        ptype = struct_path(s->stype, text_at(n->field_off), &poff);
        if (ptype <= 0) die_node(n, "field is not a struct");
        idx = gen_value(n->lhs);
        idx = coerce_to(idx, last_type, 0);
        emit(R_BOUNDS, idx, s->alen, 0, 0);
        base = struct_base(s, idx);
        if (poff) {
            int t = new_temp();
            emit(R_ADDI, base, poff, 0, t);
            return t;
        }
        return base;
    }
    die_node(n, "struct value expected");
    return 0;
}

static int member_base(Sym *s, int poff)
{
    int base;

    if (s->is_param) {
        if (!poff)
            return s->reg;
        base = new_temp();
        emit(R_ADDI, s->reg, poff, 0, base);
        return base;
    }
    base = new_temp();
    emit(R_CONST, s->offset + poff, 0, 0, base);
    return base;
}

static void copy_struct_words(int dstbase, int srcbase, int nw)
{
    for (int i = 0; i < nw; i++) {
        int v = new_temp();
        emit(R_LOADD, srcbase, i, 0, v);
        emit(R_STORED, dstbase, i, v, 0);
    }
}

static void emit_struct_call(int fid, Node *args, int dstbase)
{
    int argc = 0;

    for (Node *p = args; p; p = p->next) {
        int v, pt;
        if (argc >= sigs[fid].nparams)
            die_node(p, "too many arguments");
        pt = sigs[fid].ptype[argc];
        if (pt > 0) {
            v = gen_struct_ref(p);
        } else {
            v = gen_value(p);
            if (pt == -2) {
                if (last_type != 2)
                    die_node(p, "string argument expected");
            } else {
                v = coerce_to(v, last_type, pt < 0 ? 1 : 0);
            }
        }
        emit(R_ARG, argc++, v, 0, 0);
    }
    if (argc != sigs[fid].nparams)
        die_node(args, "wrong argument count");
    emit(R_ARG, argc++, dstbase, 0, 0);
    emit(R_CALL, fid, new_temp(), argc, 0);
}

static Node *arg_at(Node *a, int i)
{
    while (a && i-- > 0)
        a = a->next;
    return a;
}

static int arg_count(Node *a)
{
    int n = 0;
    for (; a; a = a->next)
        n++;
    return n;
}

/* Emit a value that denotes an arena offset: a literal, a char array, a
 * string variable, or a call that returns a string. */
static int gen_string_value(Node *n)
{
    Sym *s;
    int v;

    if (n->kind == N_STR) {
        v = new_temp();
        emit(R_CONST, arena_literal(n->val), 0, 0, v);
        last_type = 2;
        return v;
    }
    if (n->kind == N_VAR) {
        s = sym_find_hash(n->hash);
        if (s && s->is_char_array) {
            v = new_temp();
            emit(R_CONST, s->offset, 0, 0, v);
            last_type = 2;
            return v;
        }
        if (s && s->is_string) {
            if (!s->global) {
                last_type = 2;
                return s->reg;
            }
            v = new_temp();
            emit(R_LOADG, s->offset, 0, 0, v);
            last_type = 2;
            return v;
        }
    }
    v = gen_value(n);
    if (last_type != 2)
        die_node(n, "string expression expected");
    return v;
}

static int arith_op(int kind)
{
    switch (kind) {
    case N_ADD: return R_ADD;
    case N_SUB: return R_SUB;
    case N_MUL: return R_MUL;
    case N_DIV: return R_DIV;
    case N_MOD: return R_MOD;
    case N_LT:  return R_LT;
    case N_GT:  return R_GT;
    case N_EQ:  return R_EQ;
    case N_NE:  return R_NE;
    case N_LE:  return R_LE;
    case N_GE:  return R_GE;
    default:    return R_EQ;
    }
}

static int gen_expr(Node *n)
{
    return gen_expr_into(n, new_temp());
}

static int gen_expr_into(Node *n, int dst)
{
    int a, b;
    Sym *s;

    switch (n->kind) {
    case N_NUM:
        emit(R_CONST, n->val, 0, 0, dst);
        last_type = n->ftype;
        return dst;
    case N_VAR:
        s = sym_find_hash(n->hash);
        if (!s) {
            int v;
            if (const_lookup_hash(n->hash, &v)) {
                emit(R_CONST, v, 0, 0, dst);
                last_type = 0;
                return dst;
            }
            die_node(n, "unknown variable");
        }
        if (s->is_char_array) {
            emit(R_CONST, s->offset, 0, 0, dst);
            last_type = 2;
            return dst;
        }
        if (s->is_string) {
            if (s->global)
                emit(R_LOADG, s->offset, 0, 0, dst);
            else if (s->reg != dst)
                emit(R_MOV, s->reg, 0, 0, dst);
            last_type = 2;
            return dst;
        }
        if (s->is_char) {
            if (s->global)
                emit(R_LOADG, s->offset, 0, 0, dst);
            else if (s->reg != dst)
                emit(R_MOV, s->reg, 0, 0, dst);
            last_type = 0;
            return dst;
        }
        if (s->is_array) die_node(n, "array used as value");
        if (s->is_struct) die_node(n, "struct used as scalar");
        if (s->global) {
            emit(R_LOADG, s->offset, 0, 0, dst);
        } else if (s->reg != dst) {
            emit(R_MOV, s->reg, 0, 0, dst);
        }
        last_type = s->is_float;
        return dst;
    case N_INDEX:
        s = sym_find_hash(n->hash);
        if (!s) die_node(n, "unknown array");
        if (s->is_char_array) {
            a = gen_value(n->lhs);
            a = coerce_to(a, last_type, 0);
            emit(R_BOUNDS, a, s->alen, 0, 0);
            emit(R_LOADB, s->offset, a, 0, dst);
            last_type = 0;
            return dst;
        }
        if (!s->is_array) die_node(n, "array expected");
        if (s->is_struct) die_node(n, "struct array element needs a field");
        a = gen_value(n->lhs);
        a = coerce_to(a, last_type, 0);
        emit(R_LOADA, s->offset, a, s->alen, dst);
        last_type = 0;
        return dst;
    case N_MEMBER: {
        int poff, ptype;
        s = sym_find_hash(n->hash);
        if (!s || !s->is_struct) die_node(n, "struct variable expected");
        ptype = struct_path(s->stype, text_at(n->field_off), &poff);
        if (ptype != 0)
            die_node(n, "struct field used as scalar");
        if (s->is_param) emit(R_LOADD, s->reg, poff, 0, dst);
        else emit(R_LOADG, s->offset + poff, 0, 0, dst);
        last_type = 0;
        return dst;
    }
    case N_ASSIGN_MEMBER: {
        int poff, ptype;
        s = sym_find_hash(n->hash);
        if (!s || !s->is_struct) die_node(n, "struct variable expected");
        ptype = struct_path(s->stype, text_at(n->field_off), &poff);
        if (ptype > 0) {
            int base = member_base(s, poff), fid, src;
            if (n->lhs->kind == N_CALL) {
                fid = func_lookup_hash(n->lhs->hash);
                if (fid < 0 || sigs[fid].rtype_struct != ptype)
                    die_node(n, "struct-returning function expected");
                emit_struct_call(fid, n->lhs->args, base);
            } else {
                if (node_struct_type(n->lhs) != ptype)
                    die_node(n, "struct type mismatch");
                src = gen_struct_ref(n->lhs);
                copy_struct_words(base, src, structs[ptype - 1].size);
            }
            last_type = 0;
            return 0;
        }
        a = gen_value(n->lhs);
        a = coerce_to(a, last_type, 0);
        if (s->is_param) emit(R_STORED, s->reg, poff, a, 0);
        else emit(R_STOREG, s->offset + poff, a, 0, 0);
        last_type = 0;
        return a;
    }
    case N_MEMBER_INDEX: {
        int poff, ptype, base;
        s = sym_find_hash(n->hash);
        if (!s || !s->is_struct || !s->is_array) die_node(n, "struct array expected");
        ptype = struct_path(s->stype, text_at(n->field_off), &poff);
        if (ptype != 0)
            die_node(n, "struct field used as scalar");
        a = gen_value(n->lhs);
        a = coerce_to(a, last_type, 0);
        emit(R_BOUNDS, a, s->alen, 0, 0);
        base = struct_base(s, a);
        emit(R_LOADD, base, poff, 0, dst);
        last_type = 0;
        return dst;
    }
    case N_ASSIGN_MEMBER_INDEX: {
        int poff, ptype, base;
        s = sym_find_hash(n->hash);
        if (!s || !s->is_struct || !s->is_array) die_node(n, "struct array expected");
        ptype = struct_path(s->stype, text_at(n->field_off), &poff);
        a = gen_value(n->lhs);
        a = coerce_to(a, last_type, 0);
        emit(R_BOUNDS, a, s->alen, 0, 0);
        base = struct_base(s, a);
        if (poff) {
            int t = new_temp();
            emit(R_ADDI, base, poff, 0, t);
            base = t;
        }
        if (ptype > 0) {
            int fid, src;
            if (n->rhs->kind == N_CALL) {
                fid = func_lookup_hash(n->rhs->hash);
                if (fid < 0 || sigs[fid].rtype_struct != ptype)
                    die_node(n, "struct-returning function expected");
                emit_struct_call(fid, n->rhs->args, base);
            } else {
                if (node_struct_type(n->rhs) != ptype)
                    die_node(n, "struct type mismatch");
                src = gen_struct_ref(n->rhs);
                copy_struct_words(base, src, structs[ptype - 1].size);
            }
            last_type = 0;
            return 0;
        }
        b = gen_value(n->rhs);
        b = coerce_to(b, last_type, 0);
        emit(R_STORED, base, 0, b, 0);
        last_type = 0;
        return b;
    }
    case N_ASSIGN:
        s = sym_find_hash(n->hash);
        if (!s) die_node(n, "unknown variable");
        if (s->is_struct) {
            int nf, i, srcbase, stype, fid;
            if (s->is_array) die_node(n, "struct assignment target");
            if (n->lhs->kind == N_CALL) {
                fid = func_lookup_hash(n->lhs->hash);
                if (fid < 0 || sigs[fid].rtype_struct != s->stype)
                    die_node(n, "struct-returning function expected");
                srcbase = s->is_param ? s->reg : new_temp();
                if (!s->is_param)
                    emit(R_CONST, s->offset, 0, 0, srcbase);
                emit_struct_call(fid, n->lhs->args, srcbase);
                last_type = 0;
                return 0;
            }
            if (s->is_param) die_node(n, "struct assignment target");
            stype = node_struct_type(n->lhs);
            if (stype != s->stype)
                die_node(n, "struct type mismatch");
            srcbase = gen_struct_ref(n->lhs);
            nf = structs[s->stype - 1].size;
            for (i = 0; i < nf; i++) {
                int v = new_temp();
                emit(R_LOADD, srcbase, i, 0, v);
                emit(R_STOREG, s->offset + i, v, 0, 0);
            }
            last_type = 0;
            return 0;
        }
        if (s->is_char_array)
            die_node(n, "char array assignment; use strlcpy");
        if (s->is_string) {
            a = gen_value(n->lhs);
            if (last_type != 2)
                die_node(n, "string value expected");
            if (s->global)
                emit(R_STOREG, s->offset, a, 0, 0);
            else if (a != s->reg)
                emit(R_MOV, a, 0, 0, s->reg);
            last_type = 2;
            return a;
        }
        if (s->is_array) die_node(n, "scalar assignment expected");
        if (s->global) {
            a = gen_value(n->lhs);
            a = coerce_to(a, last_type, s->is_float);
            emit(R_STOREG, s->offset, a, 0, 0);
            last_type = s->is_float;
            return a;
        }
        if (!s->is_float && node_float_hint(n->lhs) == 0) {
            a = gen_expr_into(n->lhs, s->reg);
            last_type = 0;
            return s->reg;
        }
        a = gen_value(n->lhs);
        a = coerce_to(a, last_type, s->is_float);
        if (a != s->reg) emit(R_MOV, a, 0, 0, s->reg);
        last_type = s->is_float;
        return s->reg;
    case N_ASSIGN_INDEX:
        s = sym_find_hash(n->hash);
        if (!s) die_node(n, "unknown array");
        if (s->is_char_array) {
            a = gen_value(n->lhs);
            a = coerce_to(a, last_type, 0);
            emit(R_BOUNDS, a, s->alen, 0, 0);
            b = gen_value(n->rhs);
            b = coerce_to(b, last_type, 0);
            emit(R_STOREB, s->offset, a, b, 0);
            last_type = 0;
            return b;
        }
        if (!s->is_array) die_node(n, "array assignment expected");
        if (s->is_struct) die_node(n, "struct array element needs a field");
        a = gen_value(n->lhs);
        a = coerce_to(a, last_type, 0);
        if (n->rhs->kind == N_NUM && !n->rhs->ftype) {
            emit(R_STOREA_I, s->offset, a, n->rhs->val, s->alen);
        } else {
            b = gen_value(n->rhs);
            b = coerce_to(b, last_type, 0);
            emit(R_STOREA, s->offset, a, b, s->alen);
        }
        last_type = 0;
        return a;
    case N_PRINT:
        a = gen_value(n->lhs);
        if (last_type == 2)
            emit(R_PRINTSTR, a, 0, 0, 0);
        else
            emit(last_type ? R_PRINTFIX : R_PRINT, a, 0, 0, 0);
        return a;
    case N_CALL: {
        int argc = 0, fid, nid, nargs, hasret;
        uint64_t h = n->hash;

        if (hash_is(h, "strlen") || hash_is(h, "atoi") ||
            hash_is(h, "strcmp") || hash_is(h, "strlcpy") ||
            hash_is(h, "strlcat")) {
            int want = hash_is(h, "strcmp") ? 2 :
                       (hash_is(h, "strlcpy") || hash_is(h, "strlcat")) ? 3 : 1;
            if (arg_count(n->args) != want)
                die_node(n, "wrong string builtin argument count");
            if (hash_is(h, "strlen")) {
                int s = gen_string_value(arg_at(n->args, 0));
                emit(R_STRLEN, s, 0, 0, dst);
            } else if (hash_is(h, "atoi")) {
                int s = gen_string_value(arg_at(n->args, 0));
                emit(R_ATOI, s, 0, 0, dst);
            } else if (hash_is(h, "strcmp")) {
                int s0 = gen_string_value(arg_at(n->args, 0));
                int s1 = gen_string_value(arg_at(n->args, 1));
                emit(R_STRCMP, s0, s1, 0, dst);
            } else {
                int d = gen_string_value(arg_at(n->args, 0));
                int s = gen_string_value(arg_at(n->args, 1));
                int cap = gen_value(arg_at(n->args, 2));
                cap = coerce_to(cap, last_type, 0);
                emit(hash_is(h, "strlcpy") ? R_STRCPY : R_STRCAT,
                     d, s, cap, dst);
            }
            last_type = 0;
            return dst;
        }

        nid = native_lookup_hash(n->hash, &nargs, &hasret);
        if (nid >= 0) {
            int a0 = -1, a1 = -1, a2 = -1;
            /* Bit i: argument i is a string (an arena offset), not an int.
             * Paths are argument 0; read/write take the buffer as argument 1. */
            int strargs = 0;
            switch (nid) {
            case NAT_RUN: case NAT_OPEN: case NAT_CREATE: case NAT_UNLINK:
                strargs = 1;
                break;
            case NAT_READ: case NAT_WRITE:
                strargs = 2;
                break;
            case NAT_CAPTURE:
                strargs = 3;    /* path + buffer */
                break;
            case NAT_ISDIR:
                strargs = 1;
                break;
            case NAT_READDIR:
                strargs = 2;    /* name buffer */
                break;
            }
            for (Node *p = n->args; p; p = p->next) {
                int v = gen_value(p);
                if (strargs & (1 << argc)) {
                    if (last_type != 2)
                        die_node(p, "string argument expected");
                } else {
                    v = coerce_to(v, last_type, 0);
                }
                if (argc == 0) a0 = v;
                else if (argc == 1) a1 = v;
                else if (argc == 2) a2 = v;
                argc++;
            }
            if (argc != nargs) die_node(n, "native argument count");
            if (nid >= NAT_OPEN) {
                /* Three arguments plus a result do not fit NATIVER; stage them
                 * in the shared argument buffer and use the NATIVEC form. */
                if (nargs > 0) emit(R_ARG, 0, a0, 0, 0);
                if (nargs > 1) emit(R_ARG, 1, a1, 0, 0);
                if (nargs > 2) emit(R_ARG, 2, a2, 0, 0);
                emit(R_NATIVEC, nid, hasret ? dst : -1, 0, 0);
            } else if (hasret) {
                emit(R_NATIVER, nid, a0, a1, dst);
            } else {
                emit(R_NATIVE, nid, a0, a1, a2);
            }
            last_type = 0;
            return dst;
        }
        fid = func_lookup_hash(n->hash);
        if (fid < 0) die_node(n, "unknown function");
        if (sigs[fid].rtype_struct > 0)
            die_node(n, "struct-returning call needs an assignment target");
        for (Node *p = n->args; p; p = p->next) {
            int v, to, pt;
            if (argc >= sigs[fid].nparams) die_node(n, "too many arguments");
            pt = sigs[fid].ptype[argc];
            if (pt > 0) {
                v = gen_struct_ref(p);
            } else {
                v = gen_value(p);
                if (pt == -2) {
                    if (last_type != 2)
                        die_node(p, "string argument expected");
                } else {
                    to = pt < 0 ? 1 : 0;
                    if (to >= 0) v = coerce_to(v, last_type, to);
                }
            }
            emit(R_ARG, argc++, v, 0, 0);
        }
        if (argc != sigs[fid].nparams) die_node(n, "wrong argument count");
        emit(R_CALL, fid, dst, argc, 0);
        last_type = sigs[fid].rtype;
        return dst;
    }
    case N_STR: {
        int off = arena_literal(n->val);
        emit(R_CONST, off, 0, 0, dst);
        last_type = 2;
        return dst;
    }
    case N_PRINTF:
    case N_PUTS:
        die_node(n, "printf/puts not allowed in expression");
    case N_AND: {
        int j1, j2, jend;
        a = gen_value(n->lhs);
        if (last_type == 2)
            die_node(n, "string in logical expression");
        j1 = emit(R_JZ, a, 0, 0, 0);
        b = gen_value(n->rhs);
        if (last_type == 2)
            die_node(n, "string in logical expression");
        j2 = emit(R_JZ, b, 0, 0, 0);
        emit(R_CONST, 1, 0, 0, dst);
        jend = emit(R_JMP, 0, 0, 0, 0);
        ir[j1].b = nir;
        ir[j2].b = nir;
        emit(R_CONST, 0, 0, 0, dst);
        ir[jend].a = nir;
        last_type = 0;
        return dst;
    }
    case N_OR: {
        int j1, jend1, j2, jend2;
        a = gen_value(n->lhs);
        if (last_type == 2)
            die_node(n, "string in logical expression");
        j1 = emit(R_JZ, a, 0, 0, 0);
        emit(R_CONST, 1, 0, 0, dst);
        jend1 = emit(R_JMP, 0, 0, 0, 0);
        ir[j1].b = nir;
        b = gen_value(n->rhs);
        if (last_type == 2)
            die_node(n, "string in logical expression");
        j2 = emit(R_JZ, b, 0, 0, 0);
        emit(R_CONST, 1, 0, 0, dst);
        jend2 = emit(R_JMP, 0, 0, 0, 0);
        ir[j2].b = nir;
        emit(R_CONST, 0, 0, 0, dst);
        ir[jend1].a = nir;
        ir[jend2].a = nir;
        last_type = 0;
        return dst;
    }
    case N_ADD: case N_SUB: case N_MUL: case N_DIV: case N_MOD:
    case N_LT: case N_GT: case N_EQ: case N_NE: case N_LE: case N_GE:
        /* Destination hint removes the temporary + MOV around assignments.
         * Immediate forms are kept for definitely-int operands. */
        if (node_float_hint(n->lhs) == 0 && node_float_hint(n->rhs) == 0 &&
            n->rhs->kind == N_NUM && !n->rhs->ftype &&
            (n->kind == N_ADD || n->kind == N_SUB || n->kind == N_MUL)) {
            a = gen_value(n->lhs);
            if (n->kind == N_ADD && n->rhs->val == 1)
                emit(R_INC, a, 0, 0, dst);
            else if (n->kind == N_SUB && n->rhs->val == 1)
                emit(R_DEC, a, 0, 0, dst);
            else
                emit(n->kind == N_ADD ? R_ADDI : n->kind == N_SUB ? R_SUBI : R_MULI,
                     a, n->rhs->val, 0, dst);
            last_type = 0;
            return dst;
        }
        if (node_float_hint(n->lhs) == 0 && node_float_hint(n->rhs) == 0 &&
            n->lhs->kind == N_NUM && !n->lhs->ftype &&
            (n->kind == N_ADD || n->kind == N_MUL)) {
            b = gen_value(n->rhs);
            emit(n->kind == N_ADD ? R_ADDI : R_MULI, b, n->lhs->val, 0, dst);
            last_type = 0;
            return dst;
        }
        a = gen_value(n->lhs);
        {
            int at = last_type;
            b = gen_value(n->rhs);
            {
                int bt = last_type;
                if (at == 2 || bt == 2)
                    die_node(n, "string in arithmetic");
                if (at || bt) {
                    if (n->kind == N_MOD)
                        die_node(n, "% needs integer operands");
                    a = coerce_to(a, at, 1);
                    b = coerce_to(b, bt, 1);
                    if (n->kind == N_ADD) emit(R_ADD, a, b, 0, dst);
                    else if (n->kind == N_SUB) emit(R_SUB, a, b, 0, dst);
                    else if (n->kind == N_MUL) emit(R_FMUL, a, b, 0, dst);
                    else if (n->kind == N_DIV) emit(R_FDIV, a, b, 0, dst);
                    else {
                        emit(arith_op(n->kind), a, b, 0, dst);
                        last_type = 0;
                        return dst;
                    }
                    last_type = 1;
                    return dst;
                }
            }
        }
        emit(arith_op(n->kind), a, b, 0, dst);
        last_type = 0;
        return dst;
    default:
        a = gen_expr(n);
        if (a != dst) emit(R_MOV, a, 0, 0, dst);
        return dst;
    }
}

static int branch_is_fused(int op)
{
    return op >= R_JLT && op <= R_JNE_I;
}

/* Branch to target when the condition is false. */
static int gen_cond_false(Node *c)
{
    int a, b;

    if (c->kind == N_LT || c->kind == N_GT || c->kind == N_EQ ||
        c->kind == N_NE || c->kind == N_LE || c->kind == N_GE) {
        if (c->rhs->kind == N_NUM && !c->rhs->ftype &&
            node_float_hint(c->lhs) == 0) {
            a = gen_value(c->lhs);
            return emit(c->kind == N_LT ? R_JGE_I :
                        c->kind == N_GT ? R_JLE_I :
                        c->kind == N_EQ ? R_JNE_I :
                        c->kind == N_NE ? R_JEQ_I :
                        c->kind == N_LE ? R_JGT_I : R_JLT_I,
                        a, c->rhs->val, 0, 0);
        }
        a = gen_value(c->lhs);
        {
            int at = last_type;
            b = gen_value(c->rhs);
            if (at || last_type) {
                a = coerce_to(a, at, 1);
                b = coerce_to(b, last_type, 1);
            }
        }
        return emit(c->kind == N_LT ? R_JGE :
                    c->kind == N_GT ? R_JLE :
                    c->kind == N_EQ ? R_JNE :
                    c->kind == N_NE ? R_JEQ :
                    c->kind == N_LE ? R_JGT : R_JLT,
                    a, b, 0, 0);
    }
    a = gen_expr(c);
    return emit(R_JZ, a, 0, 0, 0);
}

static void set_branch_target(int idx, int target)
{
    if (branch_is_fused(ir[idx].op)) ir[idx].c = target;
    else ir[idx].b = target;
}

#define LOOP_MAX 16

typedef struct {
    int brk[32], nbrk;
    int cont[32], ncont;
    int cont_target;
} LoopCtx;

typedef struct {
    int brk[32], nbrk;
} SwitchCtx;

typedef struct {
    int is_switch;
    int idx;
} BreakRef;

static LoopCtx loops[LOOP_MAX];
static int nloop;
static SwitchCtx switches[8];
static int nswitch;
static BreakRef brk_stack[LOOP_MAX + 8];
static int nbrk_stack;

static void brk_push(int is_switch, int idx)
{
    if (nbrk_stack >= (int)(sizeof brk_stack / sizeof brk_stack[0]))
        die("break contexts too deep");
    brk_stack[nbrk_stack].is_switch = is_switch;
    brk_stack[nbrk_stack].idx = idx;
    nbrk_stack++;
}

static void brk_pop(void)
{
    if (nbrk_stack > 0)
        nbrk_stack--;
}

static void switch_push(void)
{
    if (nswitch >= (int)(sizeof switches / sizeof switches[0]))
        die("switch nested too deeply");
    switches[nswitch].nbrk = 0;
    brk_push(1, nswitch);
    nswitch++;
}

static void switch_end(int break_target)
{
    SwitchCtx *s = &switches[nswitch - 1];
    for (int i = 0; i < s->nbrk; i++)
        ir[s->brk[i]].a = break_target;
    nswitch--;
    brk_pop();
}

static void loop_push(int cont_target)
{
    if (nloop >= LOOP_MAX) die("loops nested too deeply");
    loops[nloop].nbrk = 0;
    loops[nloop].ncont = 0;
    loops[nloop].cont_target = cont_target;
    brk_push(0, nloop);
    nloop++;
}

static int loop_jump(int *list, int *n)
{
    int j = emit(R_JMP, 0, 0, 0, 0);
    if (*n >= 32) die("too many break/continue in one loop");
    list[(*n)++] = j;
    return j;
}

static void loop_set_cont(int target)
{
    LoopCtx *c = &loops[nloop - 1];
    for (int i = 0; i < c->ncont; i++)
        ir[c->cont[i]].a = target;
    c->ncont = 0;
    c->cont_target = target;
}

static void loop_end(int break_target)
{
    LoopCtx *c = &loops[nloop - 1];
    for (int i = 0; i < c->nbrk; i++)
        ir[c->brk[i]].a = break_target;
    for (int i = 0; i < c->ncont; i++)
        ir[c->cont[i]].a = (c->cont_target >= 0) ? c->cont_target : break_target;
    nloop--;
    brk_pop();
}

static void gen_stmt(Node *n)
{
    if (!n) return;
    switch (n->kind) {
    case N_BLOCK:
        for (Node *s = n->body; s; s = s->next) {
            int save = vreg;
            gen_stmt(s);
            vreg = save;   /* temps are dead at statement boundaries */
        }
        break;
    case N_DECL: {
        Sym *s = sym_find(text_at((uint32_t)n->hash));
        if (!s) die("declaration lost");
        if (n->ctype == 2) {
            if (n->lhs) {
                int d = new_temp();
                int src = gen_string_value(n->lhs);
                int cap = new_temp();
                int tmp = new_temp();
                emit(R_CONST, s->offset, 0, 0, d);
                emit(R_CONST, n->alen, 0, 0, cap);
                emit(R_STRCPY, d, src, cap, tmp);
            }
            break;
        }
        if (n->ctype == 3)
            emit(R_CONST, -1, 0, 0, s->reg);   /* null string */
        else if (!s->is_array && !s->is_struct)
            emit(R_CONST, 0, 0, 0, s->reg);
        if (n->lhs) {
            Node tmp;
            memset(&tmp, 0, sizeof tmp);
            tmp.kind = N_ASSIGN;
            tmp.hash = str_hash(text_at((uint32_t)n->hash));
            tmp.lhs = n->lhs;
            gen_expr(&tmp);
        }
        break;
    }
    case N_BREAK: {
        BreakRef *r;
        if (nbrk_stack == 0) die("break outside loop/switch");
        r = &brk_stack[nbrk_stack - 1];
        if (r->is_switch)
            loop_jump(switches[r->idx].brk, &switches[r->idx].nbrk);
        else
            loop_jump(loops[r->idx].brk, &loops[r->idx].nbrk);
        break;
    }
    case N_CONTINUE: {
        if (nloop == 0) die("continue outside loop");
        loop_jump(loops[nloop - 1].cont, &loops[nloop - 1].ncont);
        break;
    }
    case N_RETURN: {
        int v;
        if (cur_rstruct > 0) {
            if (n->lhs->kind == N_CALL) {
                int fid = func_lookup_hash(n->lhs->hash);
                if (fid < 0 || sigs[fid].rtype_struct != cur_rstruct)
                    die_node(n, "struct-returning function expected");
                emit_struct_call(fid, n->lhs->args, cur_ret_base);
            } else {
                int src, nw;
                if (node_struct_type(n->lhs) != cur_rstruct)
                    die_node(n, "struct return value mismatch");
                src = gen_struct_ref(n->lhs);
                nw = structs[cur_rstruct - 1].size;
                for (int i = 0; i < nw; i++) {
                    int t = new_temp();
                    emit(R_LOADD, src, i, 0, t);
                    emit(R_STORED, cur_ret_base, i, t, 0);
                }
            }
            v = new_temp();
            emit(R_CONST, 0, 0, 0, v);
            emit(R_RET, v, 0, 0, 0);
            break;
        }
        v = gen_value(n->lhs);
        if (cur_rtype == 2) {
            if (last_type != 2)
                die_node(n, "string return value expected");
        } else {
            v = coerce_to(v, last_type, cur_rtype);
        }
        emit(R_RET, v, 0, 0, 0);
        break;
    }
    case N_ASSIGN:
    case N_ASSIGN_INDEX:
    case N_ASSIGN_MEMBER:
    case N_ASSIGN_MEMBER_INDEX:
    case N_PRINT:
    case N_CALL:
        gen_expr(n);
        break;
    case N_PUTS:
        emit(R_PRINTS, n->val, 0, 0, 0);
        break;
    case N_PRINTF: {
        int off = n->val, len = n->alen, p = -1, prec = -1, after = -1;
        char conv = 0;
        for (int i = 0; i + 1 < len; i++)
            if (strpool[off + i] == '%') {
                int j = i + 1;
                if (j < len && strpool[off + j] == '.') {
                    j++;
                    prec = 0;
                    while (j < len && strpool[off + j] >= '0' && strpool[off + j] <= '9')
                        prec = prec * 10 + (strpool[off + j++] - '0');
                }
                if (j < len) {
                    conv = strpool[off + j];
                    after = j + 1;
                    p = i;
                }
                break;
            }
        if (p < 0) {
            emit(R_PRINTN, off, len, 0, 0);
        } else {
            int v;
            if (!n->args)
                die_node(n, "printf conversion needs an argument");
            if (p > 0) emit(R_PRINTN, off, p, 0, 0);
            v = gen_value(n->args);
            if (conv == 'f') {
                v = coerce_to(v, last_type, 1);
                if (prec >= 0) emit(R_PRINTFIXN, v, prec, 0, 0);
                else emit(R_PRINTFIXV, v, 0, 0, 0);
            } else if (conv == 'd' || conv == 'i' || conv == 'u') {
                v = coerce_to(v, last_type, 0);
                emit(R_PRINTV, v, 0, 0, 0);
            } else if (conv == 's') {
                if (last_type != 2)
                    die_node(n->args, "printf %s needs a string");
                emit(R_PRINTSTR, v, 0, 0, 0);
            } else {
                die("printf: unsupported conversion");
            }
            if (after >= 0 && after < len) emit(R_PRINTN, off + after, len - after, 0, 0);
        }
        break;
    }
    case N_IF: {
        int br = gen_cond_false(n->cond);
        gen_stmt(n->then);
        if (n->els) {
            int jmp = emit(R_JMP, 0, 0, 0, 0);
            set_branch_target(br, nir);
            gen_stmt(n->els);
            ir[jmp].a = nir;
        } else {
            set_branch_target(br, nir);
        }
        break;
    }
    case N_WHILE: {
        int start;
        int br;
        loop_push(nir);
        start = nir;
        br = gen_cond_false(n->cond);
        gen_stmt(n->body);
        emit(R_JMP, start, 0, 0, 0);
        set_branch_target(br, nir);
        loop_end(nir);
        break;
    }
    case N_FOR: {
        int save = vreg;
        int start, br = -1;
        if (n->lhs) {
            int s2 = vreg;
            gen_stmt(n->lhs);
            vreg = s2;
        }
        loop_push(-1);
        start = nir;
        if (n->cond)
            br = gen_cond_false(n->cond);
        gen_stmt(n->body);
        loop_set_cont(nir);
        if (n->rhs) {
            int s3 = vreg;
            gen_stmt(n->rhs);
            vreg = s3;
        }
        emit(R_JMP, start, 0, 0, 0);
        if (br >= 0)
            set_branch_target(br, nir);
        loop_end(nir);
        vreg = save;
        break;
    }
    case N_DO: {
        int save = vreg;
        int start;
        int br;
        loop_push(-1);
        start = nir;
        gen_stmt(n->body);
        loop_set_cont(nir);
        br = gen_cond_false(n->cond);
        emit(R_JMP, start, 0, 0, 0);
        set_branch_target(br, nir);
        loop_end(nir);
        vreg = save;
        break;
    }
    case N_SWITCH: {
        Node *cases[64];
        int labels[64], jumps[64];
        int ncase = 0, defidx = -1, defjump = -1, nomatch, exit;
        int v = gen_value(n->lhs);
        v = coerce_to(v, last_type, 0);

        for (Node *c = n->body; c; c = c->next) {
            if (ncase >= 64) die("too many switch cases");
            cases[ncase] = c;
            if (c->stype) defidx = ncase;
            ncase++;
        }
        for (int i = 0; i < ncase; i++) {
            if (cases[i]->stype)
                jumps[i] = -1;
            else
                jumps[i] = emit(R_JEQ_I, v, cases[i]->val, 0, 0);
        }
        if (defidx >= 0)
            defjump = emit(R_JMP, 0, 0, 0, 0);
        nomatch = emit(R_JMP, 0, 0, 0, 0);

        switch_push();
        for (int i = 0; i < ncase; i++) {
            labels[i] = nir;
            if (cases[i]->stype)
                ir[defjump].a = labels[i];
            else
                ir[jumps[i]].c = labels[i];
            for (Node *s = cases[i]->body; s; s = s->next) {
                int sv = vreg;
                gen_stmt(s);
                vreg = sv;
            }
        }
        exit = nir;
        ir[nomatch].a = exit;
        switch_end(exit);
        break;
    }
    default:
        die_node(n, "bad statement");
    }
}

/* ---- Register allocation: liveness intervals + linear scan + spill ---- */

#define NPHYS 16
#define NSCRATCH 3          /* physical 13,14,15 reserved for spill reloads/stores */

static int func_phys[VREG_MAX];
static int func_slot[VREG_MAX];
static int func_remat[VREG_MAX];
static int func_cval[VREG_MAX];
static RIR newir[IR_MAX];

static int rir_get(const RIR *q, int f)
{
    switch (f) {
    case 0: return q->a;
    case 1: return q->b;
    case 2: return q->c;
    case 3: return q->d;
    default: return 0;
    }
}

static void rir_put(RIR *q, int f, int v)
{
    switch (f) {
    case 0: q->a = v; break;
    case 1: q->b = v; break;
    case 2: q->c = v; break;
    case 3: q->d = v; break;
    }
}

/* Field indices of vreg operands used by the instruction. */
static int rir_uses(const RIR *q, int *f)
{
    switch (q->op) {
    case R_MOV: case R_JZ: case R_RET: case R_PRINT: case R_PRINTV:
        f[0] = 0; return 1;
    case R_ADD: case R_SUB: case R_MUL: case R_DIV: case R_MOD:
    case R_LT: case R_GT: case R_EQ: case R_NE: case R_LE: case R_GE:
        f[0] = 0; f[1] = 1; return 2;
    case R_STOREG: case R_LOADA: case R_ARG: f[0] = 1; return 1;
    case R_STOREA: f[0] = 1; f[1] = 2; return 2;
    case R_STOREL: f[0] = 0; return 1;
    case R_INC: case R_DEC:
    case R_ADDI: case R_SUBI: case R_MULI:
        f[0] = 0; return 1;
    case R_JLT: case R_JGE: case R_JGT: case R_JLE: case R_JEQ: case R_JNE:
        f[0] = 0; f[1] = 1; return 2;
    case R_JLT_I: case R_JGE_I: case R_JGT_I: case R_JLE_I: case R_JEQ_I: case R_JNE_I:
        f[0] = 0; return 1;
    case R_STOREA_I: f[0] = 1; return 1;
    case R_NATIVE: f[0] = 1; f[1] = 2; f[2] = 3; return 3;
    case R_NATIVER: f[0] = 1; f[1] = 2; return 2;
    case R_NATIVEC: return 0;   /* arguments come from ARG */
    case R_LOADD: f[0] = 0; return 1;
    case R_STORED: f[0] = 0; f[1] = 2; return 2;
    case R_SHLI: case R_SHRI: f[0] = 0; return 1;
    case R_FMUL: case R_FDIV: f[0] = 0; f[1] = 1; return 2;
    case R_PRINTFIX: case R_PRINTFIXV: f[0] = 0; return 1;
    case R_BOUNDS: case R_PRINTFIXN: f[0] = 0; return 1;
    case R_STRLEN: case R_ATOI: case R_PRINTSTR:
        f[0] = 0; return 1;
    case R_LOADB:
        f[0] = 1; return 1;
    case R_STOREB: f[0] = 1; f[1] = 2; return 2;
    case R_STRCMP:
        f[0] = 0; f[1] = 1; return 2;
    case R_STRCPY: case R_STRCAT:
        f[0] = 0; f[1] = 1; f[2] = 2; return 3;
    default: return 0;
    }
}

/* Field index of the vreg destination, or -1. */
static int rir_dest_field(const RIR *q)
{
    switch (q->op) {
    case R_CONST: case R_MOV: case R_ADD: case R_SUB: case R_MUL: case R_DIV: case R_MOD:
    case R_LT: case R_GT: case R_EQ: case R_NE: case R_LE: case R_GE: case R_LOADG: case R_LOADA: case R_LOADL:
    case R_INC: case R_DEC:
    case R_ADDI: case R_SUBI: case R_MULI:
        return 3;   /* d */
    case R_CALL:
        return 1;   /* return value goes to b */
    case R_NATIVER:
        return 3;   /* d */
    case R_NATIVEC:
        return 1;   /* b: dst, or -1 */
    case R_LOADD:
        return 3;   /* d */
    case R_SHLI: case R_SHRI: case R_FMUL: case R_FDIV:
    case R_LOADB: case R_STRCPY: case R_STRCAT:
    case R_STRLEN: case R_STRCMP: case R_ATOI:
        return 3;   /* d */
    default:
        return -1;
    }
}

/* Mark instruction indices that can be entered from a branch. */
static void mark_targets(char *target)
{
    memset(target, 0, IR_MAX);
    for (int i = 0; i < nir; i++) {
        int op = ir[i].op;
        int t = -1;
        if (op == R_JMP) t = ir[i].a;
        else if (op == R_JZ) t = ir[i].b;
        else if (branch_is_fused(op)) t = ir[i].c;
        if (t >= 0 && t < IR_MAX)
            target[t] = 1;
    }
}

/* Straight-line copy propagation: after `MOV src, dst`, rewrite later reads
 * of dst to src until dst/src is redefined, a basic-block boundary, or a
 * branch target.  The MOV is left in place; DCE removes it if it becomes
 * unread.  Conservative by construction: it never rewrites across a target. */
static void copy_prop(void)
{
    char target[IR_MAX];
    int i, j;

    mark_targets(target);
    for (i = 0; i < nir; i++) {
        int src, dst;

        if (ir[i].op != R_MOV)
            continue;
        src = ir[i].a;
        dst = ir[i].d;
        if (src < 0 || src >= VREG_MAX || dst < 0 || dst >= VREG_MAX)
            continue;
        if (src == dst) {
            ir[i].op = R_NOP;
            continue;
        }
        for (j = i + 1; j < nir; j++) {
            int f[3], n, k, dfield, dv;

            if (target[j])
                break;
            n = rir_uses(&ir[j], f);
            for (k = 0; k < n; k++)
                if (rir_get(&ir[j], f[k]) == dst)
                    rir_put(&ir[j], f[k], src);
            if (ir[j].op == R_JMP || ir[j].op == R_JZ ||
                ir[j].op == R_RET || ir[j].op == R_HALT ||
                branch_is_fused(ir[j].op))
                break;
            dfield = rir_dest_field(&ir[j]);
            if (dfield >= 0) {
                dv = rir_get(&ir[j], dfield);
                if (dv == dst || dv == src)
                    break;
            }
        }
    }
}

/* Straight-line dead-store elimination.  Keeps indexes stable by replacing
 * the instruction with NOP instead of removing it. */
static int rir_pure(int op);

static void peephole(void)
{
    char target[IR_MAX];
    int i, j;

    mark_targets(target);

    for (i = 0; i < nir; i++) {
        int d = rir_dest_field(&ir[i]);
        int v, f[3], n;

        if (d < 0) continue;
        if (!rir_pure(ir[i].op)) continue;   /* never drop stores/calls/prints */
        v = rir_get(&ir[i], d);
        if (v < 0 || v >= VREG_MAX || target[i]) continue;

        for (j = i + 1; j < nir; j++) {
            int dj, read = 0;

            if (target[j]) break;
            n = rir_uses(&ir[j], f);
            for (int k = 0; k < n; k++)
                if (rir_get(&ir[j], f[k]) == v) read = 1;
            if (read) break;
            dj = rir_dest_field(&ir[j]);
            if (dj >= 0 && rir_get(&ir[j], dj) == v) {
                ir[i].op = R_NOP;
                break;
            }
            if (ir[j].op == R_JMP || ir[j].op == R_JZ ||
                ir[j].op == R_RET || ir[j].op == R_HALT ||
                branch_is_fused(ir[j].op))
                break;
        }
    }
}

static int rir_pure(int op)
{
    switch (op) {
    case R_CONST: case R_MOV:
    case R_ADD: case R_SUB: case R_MUL: case R_MOD:
    case R_LT: case R_GT: case R_EQ: case R_NE: case R_LE: case R_GE:
    case R_ADDI: case R_SUBI: case R_MULI:
    case R_INC: case R_DEC:
    case R_SHLI: case R_SHRI:
    case R_LOADG: case R_LOADL:
    case R_FMUL: case R_FDIV:
    case R_LOADA: case R_LOADD: case R_LOADB:
    case R_STRLEN: case R_STRCMP: case R_ATOI:
        return 1;
    default:
        return 0;
    }
}

/* Conservative DCE: if a virtual register has no readers anywhere in the
 * function, a pure definition of it is dead.  Vregs reused for unrelated
 * values stay untouched, so this cannot drop a live definition. */
static void dce(void)
{
    int used[VREG_MAX];

    memset(used, 0, sizeof used);
    for (int i = 0; i < nir; i++) {
        int f[3], n = rir_uses(&ir[i], f);
        for (int k = 0; k < n; k++) {
            int v = rir_get(&ir[i], f[k]);
            if (v >= 0 && v < VREG_MAX)
                used[v] = 1;
        }
    }
    for (int i = 0; i < nir; i++) {
        int d = rir_dest_field(&ir[i]);
        if (d < 0 || !rir_pure(ir[i].op))
            continue;
        {
            int v = rir_get(&ir[i], d);
            if (v >= 0 && v < VREG_MAX && !used[v])
                ir[i].op = R_NOP;
        }
    }
}

/* Drop NOPs and remap branch targets.  A target that lands on a NOP maps to
 * the next real instruction; jumping to the end maps to the new end. */
static void compact(void)
{
    int map[IR_MAX + 1];
    int old_nir = nir;
    int out = 0;

    for (int i = 0; i < old_nir; i++) {
        map[i] = out;
        if (ir[i].op != R_NOP)
            newir[out++] = ir[i];
    }
    map[old_nir] = out;

    for (int i = 0; i < out; i++) {
        RIR *q = &newir[i];
        if (q->op == R_JMP && q->a >= 0 && q->a <= old_nir) q->a = map[q->a];
        else if (q->op == R_JZ && q->b >= 0 && q->b <= old_nir) q->b = map[q->b];
        else if (branch_is_fused(q->op) && q->c >= 0 && q->c <= old_nir)
            q->c = map[q->c];
    }
    memcpy(ir, newir, (size_t)out * sizeof(RIR));
    nir = out;
}

static void allocate_func(int fi, int lo, int hi)
{
    int first[VREG_MAX], last[VREG_MAX];
    int defcnt[VREG_MAX], isconst[VREG_MAX], cval[VREG_MAX];
    int order[VREG_MAX], m = 0;
    int reg_end[NPHYS];
    int *phys = func_phys, *slot = func_slot;
    int nparams = sigs[fi].nparams + (sigs[fi].rtype_struct > 0 ? 1 : 0);

    for (int v = 0; v < VREG_MAX; v++) {
        first[v] = -1; last[v] = -1; phys[v] = -1; slot[v] = -1;
        defcnt[v] = 0; isconst[v] = 1; cval[v] = 0;
        func_remat[v] = 0;
    }
    for (int pc = lo; pc < hi; pc++) {
        int f[3], n = rir_uses(&ir[pc], f);
        for (int k = 0; k < n; k++) {
            int v = rir_get(&ir[pc], f[k]);
            if (v < 0 || v >= VREG_MAX) continue;
            if (first[v] < 0) first[v] = pc;
            last[v] = pc;
        }
        int d = rir_dest_field(&ir[pc]);
        if (d >= 0) {
            int v = rir_get(&ir[pc], d);
            if (v >= 0 && v < VREG_MAX) {
                if (first[v] < 0) first[v] = pc;
                last[v] = pc;
                defcnt[v]++;
                if (ir[pc].op == R_CONST) cval[v] = ir[pc].a;
                else isconst[v] = 0;
            }
        }
    }
    for (int v = 0; v < VREG_MAX; v++) if (first[v] >= 0) order[m++] = v;
    /* sort by interval start (insertion sort, small n) */
    for (int i = 1; i < m; i++) {
        int x = order[i], j = i - 1;
        while (j >= 0 && first[order[j]] > first[x]) { order[j + 1] = order[j]; j--; }
        order[j + 1] = x;
    }
    for (int r = 0; r < NPHYS; r++) reg_end[r] = -1;
    /* parameters are precolored to r0..r(nparams-1) and reserved */
    for (int p = 0; p < nparams; p++) {
        phys[p] = p;
        reg_end[p] = hi;    /* stay reserved for the whole function */
    }
    int nspill = 0;
    for (int i = 0; i < m; i++) {
        int v = order[i];
        if (v < nparams) continue;
        int r;
        for (r = nparams; r < NPHYS - NSCRATCH; r++)
            if (reg_end[r] < first[v]) reg_end[r] = -1;   /* expired */
        for (r = nparams; r < NPHYS - NSCRATCH; r++)
            if (reg_end[r] == -1) break;
        if (r < NPHYS - NSCRATCH) {
            phys[v] = r;
            reg_end[r] = last[v];
        } else {
            phys[v] = -1;
            /* rvm has a fixed 16-slot frame and will reject slots >= 16 at
             * runtime; rcc sizes its frame from maxslot, so csc must NOT
             * reject large functions here (large files use csc -> rcc). */
            slot[v] = nspill++;
        }
    }
    for (int v = 0; v < VREG_MAX; v++)
        if (phys[v] == -1 && defcnt[v] == 1 && isconst[v] && v >= nparams) {
            func_remat[v] = 1;
            func_cval[v] = cval[v];
        }
}

static void rewrite_one(int fi)
{
    int map[IR_MAX];
    int out = 0;

    for (int pc = 0; pc < nir; pc++) {
        int extra = 0;
        map[pc] = out;
        {
            int f[3], n = rir_uses(&ir[pc], f);
            for (int k = 0; k < n; k++) {
                int v = rir_get(&ir[pc], f[k]);
                if (v >= 0 && v < VREG_MAX && func_phys[v] == -1) extra++;
            }
            int d = rir_dest_field(&ir[pc]);
            if (d >= 0) {
                int v = rir_get(&ir[pc], d);
                if (v >= 0 && v < VREG_MAX && func_phys[v] == -1) extra++;
            }
        }
        out += 1 + extra;
    }
    if (out > IR_MAX) die("RIR overflow after spill");

    out = 0;
    for (int pc = 0; pc < nir; pc++) {
        RIR q = ir[pc];
        int d, df, f[3], n;

        n = rir_uses(&q, f);
        for (int k = 0; k < n; k++) {
            int v = rir_get(&q, f[k]);
            if (v >= 0 && v < VREG_MAX && func_phys[v] == -1) {
                int sc = NPHYS - NSCRATCH + k;   /* 13 for first, 14 for second */
                if (func_remat[v])
                    newir[out++] = (RIR){ R_CONST, func_cval[v], 0, 0, sc };
                else
                    newir[out++] = (RIR){ R_LOADL, func_slot[v], 0, 0, sc };
                rir_put(&q, f[k], sc);
            } else if (v >= 0 && v < VREG_MAX) {
                rir_put(&q, f[k], func_phys[v]);
            }
        }
        d = rir_dest_field(&q);
        df = -1;
        if (d >= 0) {
            int v = rir_get(&q, d);
            if (v >= 0 && v < VREG_MAX && func_phys[v] == -1) {
                if (!func_remat[v])
                    df = func_slot[v];
                rir_put(&q, d, NPHYS - 1);       /* 15 */
            } else if (v >= 0 && v < VREG_MAX) {
                rir_put(&q, d, func_phys[v]);
            }
        }
        newir[out++] = q;
        if (df >= 0)
            newir[out++] = (RIR){ R_STOREL, NPHYS - 1, df, 0, 0 };
    }

    for (int i = 0; i < out; i++) {
        if (newir[i].op == R_JMP) newir[i].a = map[newir[i].a];
        else if (newir[i].op == R_JZ) newir[i].b = map[newir[i].b];
        else if (branch_is_fused(newir[i].op)) newir[i].c = map[newir[i].c];
    }
    memcpy(ir, newir, (size_t)out * sizeof(RIR));
    nir = out;
}

static void regalloc_one(int fi)
{
    allocate_func(fi, 0, nir);
    rewrite_one(fi);
}

/* ---- streaming output: RIR2 ------------------------------------------- */

#define OUT_HDR 28

static int out_fd;
static int out_nir;

static void put32(uint8_t *p, int v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void write_u32(int fd, int v)
{
    uint8_t b[4];
    put32(b, v);
    write_all(fd, b, 4);
}

static void write_ir(const RIR *q)
{
    uint8_t b[20];
    int v[5];
    v[0] = q->op; v[1] = q->a; v[2] = q->b; v[3] = q->c; v[4] = q->d;
    for (int k = 0; k < 5; k++)
        put32(b + k * 4, v[k]);
    write_all(out_fd, b, 20);
}

static int out_begin(const char *path)
{
    uint8_t z[OUT_HDR];
    memset(z, 0, sizeof z);
    out_fd = create(path);
    if (out_fd < 0)
        return -1;
    return write_all(out_fd, z, OUT_HDR) == OUT_HDR ? 0 : -1;
}

static int out_finish(int mainid)
{
    uint8_t h[OUT_HDR];

    for (int i = 0; i < nfuncs; i++)
        write_u32(out_fd, sigs[i].entry_pc);
    if (strsize > 0)
        write_all(out_fd, strpool, strsize);
    if (arena_next > 0)
        write_all(out_fd, arena, arena_next);

    memset(h, 0, sizeof h);
    h[0] = 'R'; h[1] = 'I'; h[2] = 'R'; h[3] = '2';
    put32(h + 4, out_nir);
    put32(h + 8, gdata_next);
    put32(h + 12, sigs[mainid].entry_pc);
    put32(h + 16, nfuncs);
    put32(h + 20, strsize);
    put32(h + 24, arena_next);

    if (lseek(out_fd, 0, 0) < 0)
        return -1;
    if (write_all(out_fd, h, OUT_HDR) != OUT_HDR)
        return -1;
    close(out_fd);
    return 0;
}

/* Skip a balanced { ... } in pass 1, without collecting strings or building
 * AST nodes.  The current token must be '{'. */
static void skip_balanced(void)
{
    int depth = 1;
    int c;

    if (!accept("{"))
        die("block expected");
    while (depth > 0) {
        c = src_get();
        if (c < 0)
            die("unterminated block");
        if (c == '"') {
            while ((c = src_get()) >= 0) {
                if (c == '\\') { src_get(); continue; }
                if (c == '"') break;
            }
        } else if (c == '/' && src_peek(0) == '/') {
            while ((c = src_peek(0)) >= 0 && c != '\n')
                src_get();
        } else if (c == '/' && src_peek(0) == '*') {
            src_get();
            while ((c = src_get()) >= 0) {
                if (c == '*' && src_peek(0) == '/') { src_get(); break; }
            }
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
        }
    }
    next();
}

/* Parse and emit one function.  The return type and name are already
 * consumed; the current token is '('.  In pass 1 the body is skipped. */
static void parse_function(int pass, int rtype, int rstruct, const char *name, int *fi)
{
    Sig *f;
    Node *body;

    expect("(");
    if (pass == 1) {
        if (nfuncs >= FUNC_MAX) die("too many functions");
        f = &sigs[nfuncs++];
        memset(f, 0, sizeof *f);
        f->rtype = rtype;
        f->rtype_struct = rstruct;
        {
            int n = (int)strlen(name) + 1;
            if (name_used + n > NAME_POOL) die("too many function names");
            f->name_off = name_used;
            memcpy(name_pool + name_used, name, (size_t)n);
            name_used += n;
        }
    } else {
        if (*fi >= nfuncs) die("function table mismatch");
        f = &sigs[*fi];
        if (strcmp(sig_name(*fi), name)) die("function order mismatch");
        if (f->rtype_struct != rstruct) die("function return type mismatch");
        f->nparams = 0;
    }

    if (!accept(")")) {
        for (;;) {
            int ptype = 0, pi, char_param = 0, char_ptr = 0;
            if (is_kw("string")) {
                next();
                ptype = -2;
            } else if (is_kw("char")) {
                next();
                char_param = 1;
                if (accept("*")) {
                    char_ptr = 1;
                    ptype = -2;
                }
            } else if (is_kw("struct")) {
                next();
                if (tok_kind != 1) die("struct type expected");
                ptype = struct_lookup(tok);
                if (ptype <= 0) die("unknown struct type");
                next();
            } else if (is_kw("float")) {
                next();
                ptype = -1;
            } else {
                if (!is_kw("int")) die("int parameter expected");
                next();
            }
            if (tok_kind != 1) die("parameter name expected");
            if (f->nparams >= 8) die("too many parameters");
            pi = f->nparams++;
            strncpy(cur_params[pi], tok, 32 - 1);
            f->ptype[pi] = ptype;
            next();
            if (char_param && !char_ptr && accept("[")) {
                expect("]");
                f->ptype[pi] = -2;
            }
            if (accept(")")) break;
            expect(",");
        }
    }

    if (pass == 1) {
        skip_balanced();
        return;
    }

    nnode = 0;
    text_used = 0;
    body = parse_stmt();

    nsyms = nglobals;
    nlocals = 0;
    nir = 0;
    for (int p = 0; p < f->nparams; p++) {
        Sym *ps = sym_add(cur_params[p], 0);
        if (f->ptype[p] > 0) {
            ps->is_struct = 1;
            ps->stype = f->ptype[p];
            ps->is_param = 1;
        } else if (f->ptype[p] == -2) {
            ps->is_string = 1;
        } else if (f->ptype[p] < 0) {
            ps->is_float = 1;
        }
    }
    if (f->rtype_struct > 0)
        nlocals++;            /* hidden destination base at r[nparams] */
    declare_locals(body);
    vreg = nlocals;
    cur_rtype = f->rtype;
    cur_rstruct = f->rtype_struct;
    cur_ret_base = f->rtype_struct > 0 ? f->nparams : -1;
    gen_stmt(body);
    {
        int z = new_temp();
        emit(R_CONST, 0, 0, 0, z);
        emit(R_RET, z, 0, 0, 0);
    }
    copy_prop();
    peephole();
    dce();
    compact();
    regalloc_one(*fi);
    f->entry_pc = out_nir;

    for (int pc = 0; pc < nir; pc++) {
        RIR q = ir[pc];
        if (q.op == R_JMP)
            q.a += out_nir;
        else if (q.op == R_JZ)
            q.b += out_nir;
        else if (branch_is_fused(q.op))
            q.c += out_nir;
        write_ir(&q);
    }
    out_nir += nir;
    (*fi)++;
}

/* pass 1: struct/global layout + function signatures, bodies skipped.
 * pass 2: function bodies parsed and emitted one at a time. */
static void parse_program(int pass)
{
    int fi = 0;

    next();
    for (;;) {
        if (tok_kind == 0)
            break;

        if (is_kw("enum")) {
            int val = 0;
            next();
            if (tok_kind == 1) next();       /* optional tag */
            expect("{");
            for (;;) {
                char cname[32];
                if (accept("}")) break;
                if (tok_kind != 1) die("enum name expected");
                strncpy(cname, tok, sizeof(cname) - 1);
                cname[sizeof(cname) - 1] = 0;
                next();
                if (accept("="))
                    val = parse_const_expr();
                if (pass == 1)
                    const_add(cname, val);
                val++;
                if (accept(","))
                    continue;
                expect("}");
                break;
            }
            expect(";");
            continue;
        }

        if (is_kw("struct")) {
            char sname[32], vname[32];
            int sid;
            next();
            if (tok_kind != 1) die("struct name expected");
            strncpy(sname, tok, sizeof(sname) - 1);
            sname[sizeof(sname) - 1] = 0;
            next();

            if (tok_kind == 3 && !strcmp(tok, "{")) {
                if (pass == 1) {
                    StructType *st;
                    if (nstructs >= STRUCT_MAX) die("too many struct types");
                    st = &structs[nstructs];
                    memset(st, 0, sizeof *st);
                    strncpy(st->name, sname, sizeof(st->name) - 1);
                    accept("{");
                    while (!accept("}")) {
                        int ftype = 0, fsize = 1;
                        if (is_kw("int")) {
                            next();
                        } else if (is_kw("struct")) {
                            int fsid;
                            next();
                            if (tok_kind != 1) die("struct type expected");
                            fsid = struct_lookup(tok);
                            if (fsid <= 0) die("unknown struct type");
                            ftype = fsid;
                            fsize = structs[fsid - 1].size;
                            next();
                        } else {
                            die("int or struct field expected");
                        }
                        if (tok_kind != 1) die("field name expected");
                        if (st->nfields >= 8) die("too many fields");
                        st->ftype[st->nfields] = ftype;
                        st->foff[st->nfields] = st->size;
                        strncpy(st->fields[st->nfields++], tok, 32 - 1);
                        st->size += fsize;
                        if (st->size > 64) die("struct too large");
                        next();
                        expect(";");
                    }
                    expect(";");
                    nstructs++;
                } else {
                    skip_balanced();
                    expect(";");
                }
                continue;
            }

            sid = struct_lookup(sname);
            if (sid <= 0) die("unknown struct type");
            if (tok_kind != 1) die("variable name expected");
            strncpy(vname, tok, sizeof(vname) - 1);
            vname[sizeof(vname) - 1] = 0;
            next();
            if (tok_kind == 3 && !strcmp(tok, "(")) {
                parse_function(pass, 0, sid, vname, &fi);
                continue;
            }
            if (accept("[")) {
                int len;
                if (tok_kind != 2) die("struct array length expected");
                len = tok_num;
                next();
                expect("]");
                if (pass == 1)
                    sym_add_struct_array(vname, sid, len, 1);
            } else if (pass == 1) {
                sym_add_struct(vname, sid, 1);
            }
            expect(";");
            continue;
        }

        if (is_kw("char") || is_kw("string")) {
            int is_str = is_kw("string");
            char name[32];
            int len = 0, has_init = 0, lit = 0;

            next();
            if (tok_kind != 1) die("variable name expected");
            strncpy(name, tok, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
            next();
            if (!is_str && accept("[")) {
                if (tok_kind != 2) die("char array length expected");
                len = tok_num;
                next();
                expect("]");
            }
            if (tok_kind == 3 && !strcmp(tok, "(")) {
                if (!is_str)
                    die("char return type is not supported");
                parse_function(pass, 2, 0, name, &fi);
                continue;
            }
            if (accept("=")) {
                if (tok_kind != 4) die("string initializer expected");
                lit = tok_num;
                has_init = 1;
                next();
            }
            expect(";");
            if (pass == 1) {
                if (is_str) {
                    if (has_init) die("global string initializer unsupported");
                    sym_add_string(name, 1);
                } else if (len) {
                    Sym *s = sym_add_char_array(name, len, 1);
                    if (has_init)
                        arena_set_literal(s->offset, len, lit);
                } else {
                    if (has_init) die("global char initializer unsupported");
                    sym_add_char(name, 1);
                }
            }
            continue;
        }

        {
            int rtype = 0;
            char name[32];

            if (is_kw("float")) { rtype = 1; next(); }
            else if (is_kw("string")) { rtype = 2; next(); }
            else {
                if (!is_kw("int")) die("int expected");
                next();
            }
            if (tok_kind != 1) die("name expected");
            strncpy(name, tok, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
            next();

            if (tok_kind == 3 && !strcmp(tok, "(")) {
                parse_function(pass, rtype, 0, name, &fi);
                continue;
            }

            /* global int/float variable */
            if (accept("[")) {
                int len;
                if (rtype) die("float arrays not supported");
                if (tok_kind != 2) die("global array length expected");
                len = tok_num;
                next();
                expect("]");
                if (pass == 1)
                    sym_add_array(name, len, 1);
            } else if (pass == 1) {
                if (rtype) sym_add_float(name, 1);
                else sym_add_global(name);
            }
            expect(";");
        }
    }

    if (pass == 2 && fi != nfuncs)
        die("function table mismatch");
}

int main(int argc, char **argv)
{
    const char *in, *out, *be = "ir";
    int ai = 1;

    if (argc < 3) {
        puts("usage: csc [-b ir|mc] input output\n");
        return 2;
    }
    if (!strcmp(argv[ai], "-b")) {
        if (argc < 5) {
            puts("usage: csc [-b ir|mc] input output\n");
            return 2;
        }
        be = argv[ai + 1];
        ai += 2;
    }
    if (ai + 1 >= argc) {
        puts("usage: csc [-b ir|mc] input output\n");
        return 2;
    }
    in = argv[ai];
    out = argv[ai + 1];

    if (strcmp(be, "ir")) {
        if (!strcmp(be, "mc"))
            puts("csc: machine-code backend not implemented yet\n");
        else
            puts("csc: unknown backend\n");
        return 1;
    }

    src_fd = open(in);
    if (src_fd < 0)
        die("cannot open input");

    parse_program(1);
    nglobals = nsyms;
    src_rewind();
    if (out_begin(out) != 0)
        die("cannot create output");
    parse_program(2);

    {
        int mainid = func_lookup("main");
        if (mainid < 0) die("main required");
        if (out_finish(mainid) != 0) die("write failed");
    }

    puts("csc: ");
    puts(in);
    puts(" -> ");
    puts(out);
    puts("\n");
    return 0;
}
