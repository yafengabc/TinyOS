/* csc3 prototype: AST -> RIR -> backend interface.
 * Host prototype, not part of the TinyOS tree yet. */
#include "lib.h"
#include <string.h>
#include <stdint.h>

TOS_STACK(16 * 1024);

#define SRC_MAX 8192
#define NODE_MAX 512
#define SYM_MAX 64
#define IR_MAX 1024
#define VREG_MAX 256

#if defined(__riscv) || defined(__arm__)
#undef SRC_MAX
#undef NODE_MAX
#undef IR_MAX
#undef VREG_MAX
#define SRC_MAX 4096
#define NODE_MAX 128
#define IR_MAX 256
#define VREG_MAX 24
#endif

typedef struct Node Node;
struct Node {
    int kind;
    int val;
    int alen;
    int stype;
    int ftype;      /* 1 = Q16.16 float literal/value */
    int ctype;      /* 1 = char, 2 = char array, 3 = string */
    int line;
    char name[32];
    char field[32];
    Node *lhs, *rhs;
    Node *cond, *then, *els, *body, *next;
    Node *args;
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
    R_NE, R_LE, R_GE
};

typedef struct {
    char name[32];
    int nparams;
    char params[8][32];
    int ptype[8];       /* 0 = int, -1 = float, -2 = string, >0 = struct type id */
    int rtype;          /* 0 = int, 1 = float, 2 = string */
    Node *body;
    int entry_pc;
} Func;

#define FUNC_MAX 32
static Func funcs[FUNC_MAX];
static int nfuncs;

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

static RIR ir[IR_MAX];
static int nir;
static int vreg;
static int last_type;
static int cur_rtype;

static void die(const char *s)
{
    puts("csc3: ");
    puts(s);
    puts(" at line ");
    putint(src_line);
    puts("\n");
    exit(1);
}

static void die_node(Node *n, const char *s)
{
    puts("csc3: ");
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
        int v = 0;
        while ((c = src_peek(0)) >= 0 && c >= '0' && c <= '9') {
            v = v * 10 + (c - '0');
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
            tok_num = v * 65536 + (frac * 65536 + scale / 2) / scale;
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
            if (strsize < (int)sizeof(strpool) - 1)
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
    if (!accept(s)) { puts("csc3: expected "); puts(s); puts("\n"); exit(1); }
}

static int is_kw(const char *s) { return tok_kind == 1 && !strcmp(tok, s); }

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

static Sym *sym_add(const char *name, int global)
{
    if (nsyms >= SYM_MAX) die("symbol table full");
    Sym *s = &syms[nsyms++];
    memset(s, 0, sizeof *s);
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->global = global;
    s->reg = global ? -1 : nlocals++;
    return s;
}

static Sym *sym_add_global(const char *name)
{
    Sym *s = sym_add(name, 1);
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

static Sym *sym_add_struct(const char *name, int stype, int global)
{
    Sym *s = sym_add(name, global);
    s->is_struct = 1;
    s->stype = stype;
    s->offset = gdata_next;
    gdata_next += structs[stype - 1].nfields;
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
    gdata_next += structs[stype - 1].nfields * len;
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
    if (vreg >= VREG_MAX) die("out of virtual registers (spill not implemented)");
    return vreg++;
}

static Node *parse_expr(void);

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
            strncpy(n->name, name, sizeof(n->name) - 1);
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
                if (tok_kind != 1) die("field name expected");
                strncpy(member, tok, sizeof(member) - 1);
                member[sizeof(member) - 1] = 0;
                next();
                n = node(N_MEMBER_INDEX);
                strncpy(n->name, name, sizeof(n->name) - 1);
                strncpy(n->field, member, sizeof(n->field) - 1);
                n->lhs = idx;
                return n;
            }
            {
                Node *n = node(N_INDEX);
                strncpy(n->name, name, sizeof(n->name) - 1);
                n->lhs = idx;
                return n;
            }
        }
        if (accept(".")) {
            char member[32];
            if (tok_kind != 1) die("field or method name expected");
            strncpy(member, tok, sizeof(member) - 1);
            member[sizeof(member) - 1] = 0;
            next();
            if (accept("(")) {
                Node *call = node(N_CALL);
                Node *base = node(N_VAR);
                Node **p = &base->next;
                strncpy(call->name, member, sizeof(call->name) - 1);
                strncpy(base->name, name, sizeof(base->name) - 1);
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
            strncpy(n->name, name, sizeof(n->name) - 1);
            strncpy(n->field, member, sizeof(n->field) - 1);
            return n;
        }
        Node *n = node(N_VAR);
        strncpy(n->name, name, sizeof(n->name) - 1);
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

static Node *fold_const(Node *n)
{
    if (!n)
        return n;
    n->lhs = fold_const(n->lhs);
    n->rhs = fold_const(n->rhs);
    if (n->lhs && n->rhs &&
        n->lhs->kind == N_NUM && n->rhs->kind == N_NUM &&
        !n->lhs->ftype && !n->rhs->ftype &&
        (n->kind == N_ADD || n->kind == N_SUB || n->kind == N_MUL ||
         n->kind == N_DIV || n->kind == N_MOD ||
         n->kind == N_LT || n->kind == N_GT || n->kind == N_EQ ||
         n->kind == N_NE || n->kind == N_LE || n->kind == N_GE ||
         n->kind == N_AND || n->kind == N_OR)) {
        int a = n->lhs->val, b = n->rhs->val, v = 0;
        if (n->kind == N_ADD) v = a + b;
        else if (n->kind == N_SUB) v = a - b;
        else if (n->kind == N_MUL) v = a * b;
        else if (n->kind == N_DIV) {
            if (b == 0)
                return n;
            v = a / b;
        } else if (n->kind == N_MOD) {
            if (b == 0)
                return n;
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
        n->ftype = 0;
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
        strncpy(n->name, load->name, sizeof(n->name) - 1);
        n->lhs = value;
        break;
    case N_MEMBER:
        n = node(N_ASSIGN_MEMBER);
        strncpy(n->name, load->name, sizeof(n->name) - 1);
        strncpy(n->field, load->field, sizeof(n->field) - 1);
        n->lhs = value;
        break;
    case N_INDEX:
        n = node(N_ASSIGN_INDEX);
        strncpy(n->name, load->name, sizeof(n->name) - 1);
        n->lhs = load->lhs;
        n->rhs = value;
        break;
    case N_MEMBER_INDEX:
        n = node(N_ASSIGN_MEMBER_INDEX);
        strncpy(n->name, load->name, sizeof(n->name) - 1);
        strncpy(n->field, load->field, sizeof(n->field) - 1);
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
            if (tok_kind != 1) die("field name expected");
            strncpy(member, tok, sizeof(member) - 1);
            member[sizeof(member) - 1] = 0;
            next();
            load = node(N_MEMBER);
            strncpy(load->name, name, sizeof(load->name) - 1);
            strncpy(load->field, member, sizeof(load->field) - 1);
        } else if (accept("[")) {
            Node *idx = parse_expr();
            expect("]");
            if (accept(".")) {
                char member[32];
                if (tok_kind != 1) die("field name expected");
                strncpy(member, tok, sizeof(member) - 1);
                member[sizeof(member) - 1] = 0;
                next();
                load = node(N_MEMBER_INDEX);
                strncpy(load->name, name, sizeof(load->name) - 1);
                strncpy(load->field, member, sizeof(load->field) - 1);
                load->lhs = idx;
            } else {
                load = node(N_INDEX);
                strncpy(load->name, name, sizeof(load->name) - 1);
                load->lhs = idx;
            }
        } else {
            load = node(N_VAR);
            strncpy(load->name, name, sizeof(load->name) - 1);
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
        strncpy(n->name, tok, sizeof(n->name) - 1);
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
        strncpy(n->name, tok, sizeof(n->name) - 1);
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
        strncpy(n->name, tok, sizeof(n->name) - 1);
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
        strncpy(n->name, tok, sizeof(n->name) - 1);
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
        strncpy(n->name, tok, sizeof(n->name) - 1);
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
            if (tok_kind != 1) die("field or method name expected");
            strncpy(member, tok, sizeof(member) - 1);
            member[sizeof(member) - 1] = 0;
            next();
            if (accept("(")) {
                Node *call = node(N_CALL);
                Node *base = node(N_VAR);
                Node **p = &base->next;
                strncpy(call->name, member, sizeof(call->name) - 1);
                strncpy(base->name, name, sizeof(base->name) - 1);
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
                strncpy(load->name, name, sizeof(load->name) - 1);
                strncpy(load->field, member, sizeof(load->field) - 1);
                tail = parse_place_tail(load, semi);
                if (tail) {
                    if (semi) expect(";");
                    return tail;
                }
            }
            expect("=");
            {
                Node *n = node(N_ASSIGN_MEMBER);
                strncpy(n->name, name, sizeof(n->name) - 1);
                strncpy(n->field, member, sizeof(n->field) - 1);
                n->lhs = parse_expr();
                if (semi) expect(";");
                return n;
            }
        }
        if (accept("(")) {
            Node *n = node(N_CALL);
            strncpy(n->name, name, sizeof(n->name) - 1);
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
                if (tok_kind != 1) die("field name expected");
                strncpy(member, tok, sizeof(member) - 1);
                member[sizeof(member) - 1] = 0;
                next();
                {
                    Node *load = node(N_MEMBER_INDEX);
                    Node *tail;
                    strncpy(load->name, name, sizeof(load->name) - 1);
                    strncpy(load->field, member, sizeof(load->field) - 1);
                    load->lhs = idx;
                    tail = parse_place_tail(load, semi);
                    if (tail) {
                        if (semi) expect(";");
                        return tail;
                    }
                }
                expect("=");
                n = node(N_ASSIGN_MEMBER_INDEX);
                strncpy(n->name, name, sizeof(n->name) - 1);
                strncpy(n->field, member, sizeof(n->field) - 1);
                n->lhs = idx;
                n->rhs = parse_expr();
                if (semi) expect(";");
                return n;
            }
            {
                Node *load = node(N_INDEX);
                Node *tail;
                strncpy(load->name, name, sizeof(load->name) - 1);
                load->lhs = idx;
                tail = parse_place_tail(load, semi);
                if (tail) {
                    if (semi) expect(";");
                    return tail;
                }
            }
            {
                Node *n = node(N_ASSIGN_INDEX);
                strncpy(n->name, name, sizeof(n->name) - 1);
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
            strncpy(n->name, name, sizeof(n->name) - 1);
            strncpy(v->name, name, sizeof(v->name) - 1);
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
        if (!strcmp(funcs[i].name, name)) return i;
    return -1;
}

/* Native ids shared with rirvm. */
enum {
    NAT_GPIO_COUNT, NAT_GPIO_MODE, NAT_GPIO_WRITE, NAT_GPIO_READ,
    NAT_ADC_COUNT, NAT_ADC_READ, NAT_PWM_COUNT, NAT_PWM_SET, NAT_PWM_OFF,
    NAT_SLEEP, NAT_UPTIME
};

static int native_lookup(const char *name, int *nargs, int *hasret)
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
    };
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (!strcmp(name, tab[i].name)) {
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
        switch (n->kind) {
        case N_BLOCK:
            declare_locals(n->body);
            break;
        case N_DECL:
            if (n->ctype == 2) sym_add_char_array(n->name, n->alen, 0);
            else if (n->ctype == 3) sym_add_string(n->name, 0);
            else if (n->ctype == 1) sym_add_char(n->name, 0);
            else if (n->stype > 0) {
                if (n->alen > 0) sym_add_struct_array(n->name, n->stype, n->alen, 0);
                else sym_add_struct(n->name, n->stype, 0);
            }
            else if (n->ftype) sym_add_float(n->name, 0);
            else if (n->alen > 0) sym_add_array(n->name, n->alen, 0);
            else sym_add(n->name, 0);
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
        Sym *s = sym_find(n->name);
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
        Sym *s = sym_find(n->name);
        if (!s) {
            int v;
            if (const_lookup(n->name, &v)) {
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
        int nf = structs[s->stype - 1].nfields;
        int t = new_temp();
        emit(R_MULI, idx, nf, 0, t);
        emit(R_ADDI, t, s->offset, 0, t);
        return t;
    }
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
        s = sym_find(n->name);
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
        s = sym_find(n->name);
        if (!s) {
            int v;
            if (const_lookup(n->name, &v)) {
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
        s = sym_find(n->name);
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
        int fi;
        s = sym_find(n->name);
        if (!s || !s->is_struct) die_node(n, "struct variable expected");
        fi = struct_field(s->stype, n->field);
        if (fi < 0) die_node(n, "unknown struct field");
        if (s->is_param) emit(R_LOADD, s->reg, fi, 0, dst);
        else emit(R_LOADG, s->offset + fi, 0, 0, dst);
        last_type = 0;
        return dst;
    }
    case N_ASSIGN_MEMBER: {
        int fi;
        s = sym_find(n->name);
        if (!s || !s->is_struct) die_node(n, "struct variable expected");
        fi = struct_field(s->stype, n->field);
        if (fi < 0) die_node(n, "unknown struct field");
        a = gen_value(n->lhs);
        a = coerce_to(a, last_type, 0);
        if (s->is_param) emit(R_STORED, s->reg, fi, a, 0);
        else emit(R_STOREG, s->offset + fi, a, 0, 0);
        last_type = 0;
        return a;
    }
    case N_MEMBER_INDEX: {
        int fi, base;
        s = sym_find(n->name);
        if (!s || !s->is_struct || !s->is_array) die_node(n, "struct array expected");
        fi = struct_field(s->stype, n->field);
        if (fi < 0) die_node(n, "unknown struct field");
        a = gen_value(n->lhs);
        a = coerce_to(a, last_type, 0);
        emit(R_BOUNDS, a, s->alen, 0, 0);
        base = struct_base(s, a);
        emit(R_LOADD, base, fi, 0, dst);
        last_type = 0;
        return dst;
    }
    case N_ASSIGN_MEMBER_INDEX: {
        int fi, base;
        s = sym_find(n->name);
        if (!s || !s->is_struct || !s->is_array) die_node(n, "struct array expected");
        fi = struct_field(s->stype, n->field);
        if (fi < 0) die_node(n, "unknown struct field");
        a = gen_value(n->lhs);
        a = coerce_to(a, last_type, 0);
        emit(R_BOUNDS, a, s->alen, 0, 0);
        base = struct_base(s, a);
        b = gen_value(n->rhs);
        b = coerce_to(b, last_type, 0);
        emit(R_STORED, base, fi, b, 0);
        last_type = 0;
        return b;
    }
    case N_ASSIGN:
        s = sym_find(n->name);
        if (!s) die_node(n, "unknown variable");
        if (s->is_struct) {
            Sym *src;
            int nf, i;
            if (s->is_array || s->is_param) die_node(n, "struct assignment target");
            if (n->lhs->kind != N_VAR) die_node(n, "struct assignment source");
            src = sym_find(n->lhs->name);
            if (!src || !src->is_struct || src->is_array || src->stype != s->stype)
                die_node(n, "struct type mismatch");
            nf = structs[s->stype - 1].nfields;
            for (i = 0; i < nf; i++) {
                int v = new_temp();
                if (src->is_param) emit(R_LOADD, src->reg, i, 0, v);
                else emit(R_LOADG, src->offset + i, 0, 0, v);
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
        s = sym_find(n->name);
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

        if (!strcmp(n->name, "strlen") || !strcmp(n->name, "atoi") ||
            !strcmp(n->name, "strcmp") || !strcmp(n->name, "strlcpy") ||
            !strcmp(n->name, "strlcat")) {
            int want = !strcmp(n->name, "strcmp") ? 2 :
                       (!strcmp(n->name, "strlcpy") || !strcmp(n->name, "strlcat")) ? 3 : 1;
            if (arg_count(n->args) != want)
                die_node(n, "wrong string builtin argument count");
            if (!strcmp(n->name, "strlen")) {
                int s = gen_string_value(arg_at(n->args, 0));
                emit(R_STRLEN, s, 0, 0, dst);
            } else if (!strcmp(n->name, "atoi")) {
                int s = gen_string_value(arg_at(n->args, 0));
                emit(R_ATOI, s, 0, 0, dst);
            } else if (!strcmp(n->name, "strcmp")) {
                int s0 = gen_string_value(arg_at(n->args, 0));
                int s1 = gen_string_value(arg_at(n->args, 1));
                emit(R_STRCMP, s0, s1, 0, dst);
            } else {
                int d = gen_string_value(arg_at(n->args, 0));
                int s = gen_string_value(arg_at(n->args, 1));
                int cap = gen_value(arg_at(n->args, 2));
                cap = coerce_to(cap, last_type, 0);
                emit(!strcmp(n->name, "strlcpy") ? R_STRCPY : R_STRCAT,
                     d, s, cap, dst);
            }
            last_type = 0;
            return dst;
        }

        nid = native_lookup(n->name, &nargs, &hasret);
        if (nid >= 0) {
            int a0 = -1, a1 = -1, a2 = -1;
            for (Node *p = n->args; p; p = p->next) {
                int v = gen_value(p);
                v = coerce_to(v, last_type, 0);
                if (argc == 0) a0 = v;
                else if (argc == 1) a1 = v;
                else if (argc == 2) a2 = v;
                argc++;
            }
            if (argc != nargs) die_node(n, "native argument count");
            if (hasret) emit(R_NATIVER, nid, a0, a1, dst);
            else emit(R_NATIVE, nid, a0, a1, a2);
            last_type = 0;
            return dst;
        }
        fid = func_lookup(n->name);
        if (fid < 0) die_node(n, "unknown function");
        for (Node *p = n->args; p; p = p->next) {
            int v, to, pt;
            if (argc >= funcs[fid].nparams) die_node(n, "too many arguments");
            pt = funcs[fid].ptype[argc];
            v = gen_value(p);
            if (pt == -2) {
                if (last_type != 2)
                    die_node(p, "string argument expected");
            } else {
                to = pt < 0 ? 1 : pt > 0 ? -1 : 0;
                if (to >= 0) v = coerce_to(v, last_type, to);
            }
            emit(R_ARG, argc++, v, 0, 0);
        }
        if (argc != funcs[fid].nparams) die_node(n, "wrong argument count");
        emit(R_CALL, fid, dst, argc, 0);
        last_type = funcs[fid].rtype;
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
        Sym *s = sym_find(n->name);
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
            strncpy(tmp.name, n->name, sizeof(tmp.name) - 1);
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
        int v = gen_value(n->lhs);
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

/* Straight-line dead-store elimination.  Keeps indexes stable by replacing
 * the instruction with NOP instead of removing it. */
static int rir_pure(int op);

static void peephole(void)
{
    char target[IR_MAX];
    int i, j;

    memset(target, 0, sizeof target);
    for (i = 0; i < nir; i++) {
        int op = ir[i].op;
        if (op == R_JMP) target[ir[i].a] = 1;
        else if (op == R_JZ) target[ir[i].b] = 1;
        else if (op == R_CALL) target[ir[i].a] = 1;
        else if (branch_is_fused(op)) target[ir[i].c] = 1;
    }

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
            if (ir[j].op == R_JMP || ir[j].op == R_RET ||
                ir[j].op == R_HALT || branch_is_fused(ir[j].op))
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

static void allocate_func(int fi, int lo, int hi)
{
    int first[VREG_MAX], last[VREG_MAX];
    int defcnt[VREG_MAX], isconst[VREG_MAX], cval[VREG_MAX];
    int order[VREG_MAX], m = 0;
    int reg_end[NPHYS];
    int *phys = func_phys, *slot = func_slot;
    int nparams = funcs[fi].nparams;

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
        write_u32(out_fd, funcs[i].entry_pc);
    if (strsize > 0)
        write_all(out_fd, strpool, strsize);
    if (arena_next > 0)
        write_all(out_fd, arena, arena_next);

    memset(h, 0, sizeof h);
    h[0] = 'R'; h[1] = 'I'; h[2] = 'R'; h[3] = '2';
    put32(h + 4, out_nir);
    put32(h + 8, gdata_next);
    put32(h + 12, funcs[mainid].entry_pc);
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
static void parse_function(int pass, int rtype, const char *name, int *fi)
{
    Func *f;
    Node *body;

    expect("(");
    if (pass == 1) {
        if (nfuncs >= FUNC_MAX) die("too many functions");
        f = &funcs[nfuncs++];
        memset(f, 0, sizeof *f);
        f->rtype = rtype;
        strncpy(f->name, name, sizeof(f->name) - 1);
    } else {
        if (*fi >= nfuncs) die("function table mismatch");
        f = &funcs[*fi];
        if (strcmp(f->name, name)) die("function order mismatch");
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
            strncpy(f->params[pi], tok, 32 - 1);
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
    body = parse_stmt();

    nsyms = nglobals;
    nlocals = 0;
    nir = 0;
    for (int p = 0; p < f->nparams; p++) {
        Sym *ps = sym_add(f->params[p], 0);
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
    declare_locals(body);
    vreg = nlocals;
    cur_rtype = f->rtype;
    gen_stmt(body);
    {
        int z = new_temp();
        emit(R_CONST, 0, 0, 0, z);
        emit(R_RET, z, 0, 0, 0);
    }
    peephole();
    dce();
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
                        if (!is_kw("int")) die("int field expected");
                        next();
                        if (tok_kind != 1) die("field name expected");
                        if (st->nfields >= 8) die("too many fields");
                        strncpy(st->fields[st->nfields++], tok, 32 - 1);
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
                parse_function(pass, 2, name, &fi);
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
                parse_function(pass, rtype, name, &fi);
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
        puts("usage: csc3 [-b ir|mc] input output\n");
        return 2;
    }
    if (!strcmp(argv[ai], "-b")) {
        be = argv[ai + 1];
        ai += 2;
    }
    in = argv[ai];
    out = argv[ai + 1];

    if (strcmp(be, "ir")) {
        if (!strcmp(be, "mc"))
            puts("csc3: machine-code backend not implemented yet\n");
        else
            puts("csc3: unknown backend\n");
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

    puts("csc3: ");
    puts(in);
    puts(" -> ");
    puts(out);
    puts("\n");
    return 0;
}
