/* csc: C-like subset -> AMX core P-code.
 *
 * int/void/struct, globals, locals (declared at function top), 1D arrays,
 * functions with scalar/struct parameters, recursion, if/while/for,
 * for-in ranges, break/continue, print.
 * Struct parameters are passed by reference (no pointer syntax); a.f(x)
 * is f(a,x).
 */
#include "lib.h"
#include <string.h>
#include <stdint.h>

TOS_STACK(16 * 1024);

#define CODE_MAX 2048
#define DATA_MAX 4096
#define FIX_ONE 65536
#define NAME_MAX 32
#define FIELD_MAX 16
#define STRUCT_MAX 8
#define FUNC_MAX 16
#define PARAM_MAX 8
#define SYM_MAX 48
#define NAT_MAX 48
#define STR_MAX 128

static uint32_t code[CODE_MAX];
static int code_len;
static uint8_t data[DATA_MAX];
static int data_len;

static char src[8192];
static int src_len, src_pos;
static char tok[STR_MAX];
static int tok_kind;          /* 0 EOF, 1 id, 2 num, 3 string, 4 punct */
static int tok_num;
static char tok_str[STR_MAX];
static int tok_strlen;

typedef struct {
    int pos, kind, num, strlen;
    char tok[STR_MAX];
    char str[STR_MAX];
} LexState;

typedef struct Type {
    int kind;                 /* 0 int, 1 void, 2 struct, 3 array */
    int size;
    int sidx;
    int elem_kind;
    int elem_size;
    int elem_sidx;
    int array_len;
} Type;

typedef struct Field {
    char name[32];
    Type type;
    int off;
} Field;

typedef struct StructDef {
    char name[32];
    Field fields[FIELD_MAX];
    int nfields;
    int size;
    int defined;
} StructDef;

typedef struct Sym {
    char name[32];
    Type type;
    int kind;                 /* 0 global, 1 local, 2 param */
    int off;
    int is_ref;
} Sym;

typedef struct Func {
    char name[32];
    Type ret;
    int nparams;
    char pname[PARAM_MAX][32];
    Type ptype[PARAM_MAX];
    LexState body;
    int label;
    int defined;
} Func;

typedef struct {
    char name[32];
    int value;
} ConstDef;

typedef struct {
    char name[32];
    Type type;
    char sname[32];
    int pending;
} Alias;

static StructDef structs[STRUCT_MAX];
static int nstructs;
static Func funcs[FUNC_MAX];
static int nfuncs;
static ConstDef consts[32];
static int nconsts;
static Alias aliases[16];
static int naliases;
static Sym globals[SYM_MAX];
static int nglobals;
static Sym locals[SYM_MAX];
static int nlocals;
static Func *cur_func;
static int main_func = -1;
static int last_kind;             /* Type kind of the last expression value */

static const char *natives[NAT_MAX];
static char native_names[NAT_MAX][32];
static int nnatives;

static int loop_depth;
static int break_depth;
static int cont_fix[128], cont_depth[128], ncont;
static int brk_fix[128], brk_depth[128], nbrk;

static int call_operand[128], call_target[128], ncall;

static void patch(int at, int target);

static void die(const char *msg)
{
    puts("csc: ");
    puts(msg);
    puts(" at ");
    putint(src_pos);
    puts("\n");
    exit(1);
}

static void next(void)
{
    while (src_pos < src_len) {
        char c = src[src_pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            src_pos++;
        } else if (c == '/' && src_pos + 1 < src_len && src[src_pos + 1] == '/') {
            while (src_pos < src_len && src[src_pos] != '\n')
                src_pos++;
        } else if (c == '/' && src_pos + 1 < src_len && src[src_pos + 1] == '*') {
            src_pos += 2;
            while (src_pos + 1 < src_len &&
                   !(src[src_pos] == '*' && src[src_pos + 1] == '/'))
                src_pos++;
            src_pos += 2;
        } else {
            break;
        }
    }
    if (src_pos >= src_len) {
        tok_kind = 0;
        tok[0] = 0;
        return;
    }
    char c = src[src_pos];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
        int n = 0;
        while (src_pos < src_len) {
            c = src[src_pos];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_'))
                break;
            if (n < STR_MAX - 1)
                tok[n++] = c;
            src_pos++;
        }
        tok[n] = 0;
        tok_kind = 1;
    } else if (c >= '0' && c <= '9') {
        int n = 0;
        int intpart = 0;
        while (src_pos < src_len && src[src_pos] >= '0' && src[src_pos] <= '9') {
            intpart = intpart * 10 + (src[src_pos] - '0');
            if (n < STR_MAX - 1)
                tok[n++] = src[src_pos];
            src_pos++;
        }
        if (src_pos + 1 < src_len && src[src_pos] == '.' &&
            src[src_pos + 1] >= '0' && src[src_pos + 1] <= '9') {
            int frac = 0, scale = 1, fd = 0;
            if (n < STR_MAX - 1)
                tok[n++] = '.';
            src_pos++;
            while (src_pos < src_len && src[src_pos] >= '0' &&
                   src[src_pos] <= '9') {
                if (fd < 4) {
                    frac = frac * 10 + (src[src_pos] - '0');
                    scale *= 10;
                    fd++;
                }
                if (n < STR_MAX - 1)
                    tok[n++] = src[src_pos];
                src_pos++;
            }
            if (intpart > 32767)
                die("float literal out of range");
            tok_num = intpart * FIX_ONE +
                      (frac * FIX_ONE + scale / 2) / scale;
            tok_kind = 6;
        } else {
            tok_num = intpart;
            tok_kind = 2;
        }
        tok[n] = 0;
    } else if (c == '"') {
        int n = 0;
        src_pos++;
        while (src_pos < src_len && src[src_pos] != '"') {
            char ch = src[src_pos++];
            if (ch == '\\' && src_pos < src_len) {
                ch = src[src_pos++];
                if (ch == 'n') ch = '\n';
                else if (ch == 't') ch = '\t';
                else if (ch == 'r') ch = '\r';
            }
            if (n < STR_MAX - 1)
                tok_str[n++] = ch;
        }
        if (src_pos < src_len)
            src_pos++;
        tok_str[n] = 0;
        tok_strlen = n;
        tok_kind = 3;
    } else if (c == '\'') {
        int v;
        src_pos++;
        if (src_pos >= src_len)
            die("unterminated character literal");
        v = (unsigned char)src[src_pos++];
        if (v == '\\' && src_pos < src_len) {
            char ch = src[src_pos++];
            if (ch == 'n') v = '\n';
            else if (ch == 't') v = '\t';
            else if (ch == 'r') v = '\r';
            else if (ch == '0') v = 0;
            else if (ch == '\\') v = '\\';
            else if (ch == '\'') v = '\'';
            else if (ch == '"') v = '"';
            else v = (unsigned char)ch;
        }
        if (src_pos < src_len && src[src_pos] == '\'')
            src_pos++;
        else
            die("unterminated character literal");
        tok_num = v;
        tok[0] = 0;
        tok_kind = 2;
    } else {
        char a = src[src_pos++];
        char b = src_pos < src_len ? src[src_pos] : 0;
        char c = (src_pos + 1 < src_len) ? src[src_pos + 1] : 0;
        if ((a == '<' || a == '>') && b == a && c == '=') {
            tok[0] = a; tok[1] = a; tok[2] = '='; tok[3] = 0;
            src_pos += 2;
        } else if (a == '<' && b == '<') {
            tok[0] = '<'; tok[1] = '<'; tok[2] = 0;
            src_pos++;
        } else if (a == '>' && b == '>') {
            tok[0] = '>'; tok[1] = '>'; tok[2] = 0;
            src_pos++;
        } else if (a == '&' && b == '&') {
            tok[0] = '&'; tok[1] = '&'; tok[2] = 0;
            src_pos++;
        } else if (a == '+' && b == '+') {
            tok[0] = '+'; tok[1] = '+'; tok[2] = 0;
            src_pos++;
        } else if (a == '-' && b == '-') {
            tok[0] = '-'; tok[1] = '-'; tok[2] = 0;
            src_pos++;
        } else if (a == '|' && b == '|') {
            tok[0] = '|'; tok[1] = '|'; tok[2] = 0;
            src_pos++;
        } else if (a == '.' && b == '.') {
            tok[0] = '.'; tok[1] = '.'; tok[2] = 0;
            src_pos++;
        } else if ((a == '<' || a == '>' || a == '=' || a == '!' ||
                    a == '+' || a == '-' || a == '*' || a == '/' ||
                    a == '%' || a == '&' || a == '|' || a == '^') && b == '=') {
            tok[0] = a; tok[1] = '='; tok[2] = 0;
            src_pos++;
        } else {
            tok[0] = a; tok[1] = 0;
        }
        tok_kind = 4;
    }
}

static void lex_save(LexState *s)
{
    s->pos = src_pos;
    s->kind = tok_kind;
    s->num = tok_num;
    s->strlen = tok_strlen;
    memcpy(s->tok, tok, sizeof tok);
    memcpy(s->str, tok_str, sizeof tok_str);
}

static void lex_restore(const LexState *s)
{
    src_pos = s->pos;
    tok_kind = s->kind;
    tok_num = s->num;
    tok_strlen = s->strlen;
    memcpy(tok, s->tok, sizeof tok);
    memcpy(tok_str, s->str, sizeof tok_str);
}

static int accept(const char *s)
{
    if (tok_kind == 4 && strcmp(tok, s) == 0) {
        next();
        return 1;
    }
    return 0;
}

static void expect(const char *s)
{
    if (!accept(s)) {
        puts("csc: expected ");
        puts(s);
        puts(", got ");
        puts(tok);
        puts(" at ");
        putint(src_pos);
        puts("\n");
        exit(1);
    }
}

static void emit(uint32_t w)
{
    if (code_len >= CODE_MAX)
        die("code too large");
    code[code_len++] = w;
}

static int emit_jump(uint32_t op)
{
    emit(op);
    int at = code_len;
    emit(0);
    return at;
}

static void patch(int at, int target)
{
    code[at] = (uint32_t)((target - at + 1) * 4);
}

static int label(void)
{
    return code_len;
}

static int align4(int v)
{
    return (v + 3) & ~3;
}

static int native_slot(const char *name)
{
    for (int i = 0; i < nnatives; i++)
        if (strcmp(natives[i], name) == 0)
            return i;
    if (nnatives >= NAT_MAX)
        die("too many natives");
    strncpy(native_names[nnatives], name, sizeof(native_names[0]) - 1);
    native_names[nnatives][sizeof(native_names[0]) - 1] = 0;
    natives[nnatives] = native_names[nnatives];
    return nnatives++;
}

static Type type_int(void)
{
    Type t;
    memset(&t, 0, sizeof t);
    t.kind = 0;
    t.size = 4;
    return t;
}

static Type type_void(void)
{
    Type t;
    memset(&t, 0, sizeof t);
    t.kind = 1;
    return t;
}

static Type type_char(void)
{
    Type t;
    memset(&t, 0, sizeof t);
    t.kind = 4;
    t.size = 1;                   /* one byte, not one cell */
    return t;
}

static Type type_string(void)
{
    Type t;
    memset(&t, 0, sizeof t);
    t.kind = 5;
    t.size = 4;
    return t;
}

static Type type_float(void)
{
    Type t;
    memset(&t, 0, sizeof t);
    t.kind = 6;                   /* Q16.16 fixed point, not IEEE float */
    t.size = 4;
    return t;
}

static Type type_struct(int idx)
{
    Type t;
    memset(&t, 0, sizeof t);
    t.kind = 2;
    t.sidx = idx;
    t.size = structs[idx].size;
    return t;
}

static Type type_array(Type elem, int n)
{
    Type t;
    memset(&t, 0, sizeof t);
    t.kind = 3;
    t.elem_kind = elem.kind;
    t.elem_size = elem.size;
    t.elem_sidx = elem.sidx;
    t.array_len = n;
    t.size = elem.size * n;
    return t;
}

static int find_struct(const char *name)
{
    for (int i = 0; i < nstructs; i++)
        if (structs[i].defined && strcmp(structs[i].name, name) == 0)
            return i;
    return -1;
}

static int find_const(const char *name)
{
    for (int i = 0; i < nconsts; i++)
        if (strcmp(consts[i].name, name) == 0)
            return i;
    return -1;
}

static int find_alias(const char *name)
{
    for (int i = 0; i < naliases; i++)
        if (strcmp(aliases[i].name, name) == 0)
            return i;
    return -1;
}

static int find_field(int sidx, const char *name)
{
    for (int i = 0; i < structs[sidx].nfields; i++)
        if (strcmp(structs[sidx].fields[i].name, name) == 0)
            return i;
    return -1;
}

static Func *find_func(const char *name)
{
    for (int i = 0; i < nfuncs; i++)
        if (strcmp(funcs[i].name, name) == 0)
            return &funcs[i];
    return NULL;
}

static int func_index(Func *f)
{
    return (int)(f - funcs);
}

static int is_type_start(void)
{
    if (tok_kind != 1)
        return 0;
    if (strcmp(tok, "int") == 0 || strcmp(tok, "void") == 0 ||
        strcmp(tok, "struct") == 0 || strcmp(tok, "char") == 0 ||
        strcmp(tok, "string") == 0 || strcmp(tok, "float") == 0)
        return 1;
    return find_alias(tok) >= 0;
}

static Type parse_type(void)
{
    if (tok_kind == 1 && strcmp(tok, "int") == 0) {
        next();
        return type_int();
    }
    if (tok_kind == 1 && strcmp(tok, "void") == 0) {
        next();
        return type_void();
    }
    if (tok_kind == 1 && strcmp(tok, "char") == 0) {
        next();
        return type_char();
    }
    if (tok_kind == 1 && strcmp(tok, "string") == 0) {
        next();
        return type_string();
    }
    if (tok_kind == 1 && strcmp(tok, "float") == 0) {
        next();
        return type_float();
    }
    if (tok_kind == 1 && strcmp(tok, "struct") == 0) {
        next();
        if (tok_kind != 1)
            die("struct name expected");
        int si = find_struct(tok);
        if (si < 0)
            die("unknown struct");
        next();
        return type_struct(si);
    }
    if (tok_kind == 1) {
        int ai = find_alias(tok);
        if (ai >= 0) {
            Type t = aliases[ai].type;
            if (aliases[ai].pending) {
                int si = find_struct(aliases[ai].sname);
                if (si < 0)
                    die("incomplete struct type");
                t = type_struct(si);
            }
            next();
            return t;
        }
    }
    die("type expected");
    return type_int();
}

static Sym *find_sym(const char *name)
{
    for (int i = 0; i < nlocals; i++)
        if (strcmp(locals[i].name, name) == 0)
            return &locals[i];
    for (int i = 0; i < nglobals; i++)
        if (strcmp(globals[i].name, name) == 0)
            return &globals[i];
    return NULL;
}

static Sym *new_global(const char *name, Type t)
{
    if (nglobals >= SYM_MAX)
        die("too many globals");
    Sym *s = &globals[nglobals++];
    memset(s, 0, sizeof *s);
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->type = t;
    s->kind = 0;
    s->off = data_len;
    data_len += t.size;
    data_len = align4(data_len);
    if (data_len > DATA_MAX)
        die("data too large");
    return s;
}

static Sym *new_param(const char *name, Type t, int off, int is_ref)
{
    Sym *s = &locals[nlocals++];
    memset(s, 0, sizeof *s);
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->type = t;
    s->kind = 2;
    s->off = off;
    s->is_ref = is_ref;
    return s;
}

static Sym *new_local(const char *name, Type t, int *slots)
{
    int cells = (t.size + 3) / 4;
    if (cells < 1)
        cells = 1;
    *slots += cells;
    Sym *s = &locals[nlocals++];
    memset(s, 0, sizeof *s);
    strncpy(s->name, name, sizeof(s->name) - 1);
    s->type = t;
    s->kind = 1;
    s->off = -4 * (*slots);
    return s;
}

static int string_slot(const char *s, int len)
{
    int off = data_len;
    if (data_len + len + 1 > DATA_MAX)
        die("data too large");
    for (int i = 0; i < len; i++)
        data[data_len++] = (uint8_t)s[i];
    data[data_len++] = 0;
    data_len = align4(data_len);
    return off;
}

static int parse_const(void)
{
    int neg = 0;
    if (accept("-"))
        neg = 1;
    int v;
    if (tok_kind == 2) {
        v = tok_num;
        next();
    } else if (tok_kind == 1) {
        int ci = find_const(tok);
        if (ci < 0)
            die("constant expected");
        v = consts[ci].value;
        next();
    } else {
        die("constant initializer expected");
        return 0;
    }
    return neg ? -v : v;
}

static void skip_body(void)
{
    if (!(tok_kind == 4 && strcmp(tok, "{") == 0))
        die("{ expected");
    int depth = 0;
    do {
        if (tok_kind == 0)
            die("unterminated body");
        if (tok_kind == 4 && strcmp(tok, "{") == 0)
            depth++;
        else if (tok_kind == 4 && strcmp(tok, "}") == 0)
            depth--;
        next();
    } while (depth > 0);
}

static void scan_struct(void)
{
    next();
    if (tok_kind != 1)
        die("struct name expected");
    if (nstructs >= STRUCT_MAX)
        die("too many structs");
    StructDef *sd = &structs[nstructs];
    memset(sd, 0, sizeof *sd);
    strncpy(sd->name, tok, sizeof(sd->name) - 1);
    next();
    expect("{");
    int off = 0;
    while (!(tok_kind == 4 && strcmp(tok, "}") == 0)) {
        Type t = parse_type();
        if (tok_kind != 1)
            die("field name expected");
        char fname[32];
        strncpy(fname, tok, sizeof(fname) - 1);
        fname[sizeof(fname) - 1] = 0;
        next();
        if (accept("[")) {
            if (tok_kind != 2)
                die("array length expected");
            int n = tok_num;
            next();
            expect("]");
            t = type_array(t, n);
        }
        expect(";");
        if (sd->nfields >= FIELD_MAX)
            die("too many fields");
        Field *f = &sd->fields[sd->nfields++];
        strncpy(f->name, fname, sizeof(f->name) - 1);
        f->type = t;
        off = align4(off);
        f->off = off;
        off += t.size;
    }
    expect("}");
    expect(";");
    sd->size = align4(off);
    sd->defined = 1;
    nstructs++;
}

static void scan_enum(void)
{
    next();                           /* enum */
    if (tok_kind == 1)
        next();                       /* optional tag */
    expect("{");
    int val = 0;
    while (!(tok_kind == 4 && strcmp(tok, "}") == 0)) {
        if (tok_kind != 1)
            die("enumerator expected");
        if (nconsts >= 32)
            die("too many constants");
        ConstDef *c = &consts[nconsts];
        memset(c, 0, sizeof *c);
        strncpy(c->name, tok, sizeof(c->name) - 1);
        next();
        if (accept("="))
            val = parse_const();
        c->value = val++;
        nconsts++;
        if (!accept(","))
            break;
    }
    expect("}");
    expect(";");
}

static void scan_typedef(void)
{
    next();                           /* typedef */
    Type t;
    char tag[32];
    int pending = 0;
    tag[0] = 0;
    if (tok_kind == 1 && strcmp(tok, "struct") == 0) {
        next();
        if (tok_kind != 1)
            die("struct name expected");
        strncpy(tag, tok, sizeof(tag) - 1);
        tag[sizeof(tag) - 1] = 0;
        int si = find_struct(tag);
        next();
        if (si < 0) {
            pending = 1;
            memset(&t, 0, sizeof t);
            t.kind = 2;
        } else {
            t = type_struct(si);
        }
    } else {
        t = parse_type();
    }
    if (tok_kind != 1)
        die("typedef name expected");
    if (naliases >= 16)
        die("too many typedefs");
    Alias *a = &aliases[naliases++];
    memset(a, 0, sizeof *a);
    strncpy(a->name, tok, sizeof(a->name) - 1);
    a->type = t;
    a->pending = pending;
    if (pending)
        strncpy(a->sname, tag, sizeof(a->sname) - 1);
    next();
    expect(";");
}

static void scan_global_or_func(void)
{
    Type t = parse_type();
    if (tok_kind != 1)
        die("name expected");
    char name[32];
    strncpy(name, tok, sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    next();

    if (accept("(")) {
        if (nfuncs >= FUNC_MAX)
            die("too many functions");
        Func *f = &funcs[nfuncs];
        memset(f, 0, sizeof *f);
        strncpy(f->name, name, sizeof(f->name) - 1);
        f->ret = t;
        if (!(tok_kind == 4 && strcmp(tok, ")") == 0)) {
            do {
                if (f->nparams >= PARAM_MAX)
                    die("too many parameters");
                Type pt = parse_type();
                if (tok_kind != 1)
                    die("parameter name expected");
                strncpy(f->pname[f->nparams], tok,
                        sizeof(f->pname[0]) - 1);
                next();
                if (accept("[")) {
                    if (!(tok_kind == 4 && strcmp(tok, "]") == 0)) {
                        if (tok_kind != 2)
                            die("array length expected");
                        next();
                    }
                    expect("]");
                    pt = type_array(pt, 0);
                    pt.size = 4;          /* one reference slot */
                }
                f->ptype[f->nparams] = pt;
                f->nparams++;
            } while (accept(","));
        }
        expect(")");
        lex_save(&f->body);
        skip_body();
        f->defined = 1;
        if (strcmp(name, "main") == 0)
            main_func = nfuncs;
        nfuncs++;
        return;
    }

    if (accept("[")) {
        if (tok_kind != 2)
            die("array length expected");
        int n = tok_num;
        next();
        expect("]");
        t = type_array(t, n);
    }
    Sym *s = new_global(name, t);
    if (accept("=")) {
        if (t.kind != 0)
            die("only scalar global initializer");
        int v = parse_const();
        data[s->off] = (uint8_t)v;
        data[s->off + 1] = (uint8_t)(v >> 8);
        data[s->off + 2] = (uint8_t)(v >> 16);
        data[s->off + 3] = (uint8_t)(v >> 24);
    }
    expect(";");
}

static void scan_top(void)
{
    next();
    while (tok_kind != 0) {
        if (tok_kind == 1 && strcmp(tok, "struct") == 0) {
            LexState save;
            lex_save(&save);
            next();
            int is_def = 0;
            if (tok_kind == 1) {
                next();
                if (tok_kind == 4 && strcmp(tok, "{") == 0)
                    is_def = 1;
            }
            lex_restore(&save);
            if (is_def)
                scan_struct();
            else
                scan_global_or_func();
        } else if (tok_kind == 1 && strcmp(tok, "enum") == 0) {
            scan_enum();
        } else if (tok_kind == 1 && strcmp(tok, "typedef") == 0) {
            scan_typedef();
        } else if (is_type_start()) {
            scan_global_or_func();
        } else {
            die("top-level declaration expected");
        }
    }
}

static void gen_addr(const Sym *s)
{
    if (s->kind == 0) {
        emit(9);                   /* CONST_PRI data offset */
        emit((uint32_t)s->off);
    } else if (s->kind == 2 && s->is_ref) {
        emit(3);                   /* LOAD_S_PRI: param slot holds address */
        emit((uint32_t)s->off);
    } else {
        emit(11);                  /* ADDR_PRI: frm+off */
        emit((uint32_t)s->off);
    }
}

static void expr(void);

static Type designator(int *is_lv);

static void string_arg(void);

static void emit_native_1_pri(const char *name);
static void emit_native_2_float_int(const char *name, int arg2);
static void emit_fix16_toint(void);
static void emit_fix16_binop(const char *name);
static void emit_shift_pri(int n);
static void convert_numeric(Type target);

static void push_value(void)
{
    emit(22);                      /* PUSH_PRI */
}

static void call_func(Func *f, int first)
{
    for (int i = first; i < f->nparams; i++) {
        if (f->ptype[i].kind == 2) {
            int lv;
            Type t = designator(&lv);
            (void)t;
            if (!lv)
                die("struct argument must be a variable/member");
            push_value();
        } else if (f->ptype[i].kind == 3) {
            int lv;
            Type t = designator(&lv);
            (void)t;
            if (!lv)
                die("array argument must be a variable/member");
            push_value();
        } else if (f->ptype[i].kind == 5) {
            string_arg();
            push_value();
        } else {
            expr();
            if (f->ptype[i].kind == 6 || f->ptype[i].kind == 0 ||
                f->ptype[i].kind == 4)
                convert_numeric(f->ptype[i]);
            push_value();
        }
        if (i + 1 < f->nparams)
            expect(",");
    }
    emit(33);                      /* CALL */
    int at = code_len;
    emit(0);
    call_operand[ncall] = at;
    call_target[ncall] = func_index(f);
    ncall++;
    emit(28);                      /* STACK */
    emit((uint32_t)(f->nparams * 4));
}

static Type designator(int *is_lv)
{
    if (tok_kind != 1)
        die("variable expected");
    Sym *s = find_sym(tok);
    if (!s)
        die("unknown variable");
    next();
    gen_addr(s);
    Type t = s->type;
    *is_lv = 1;

    for (;;) {
        if (accept(".")) {
            if (tok_kind != 1)
                die("member name expected");
            char name[32];
            strncpy(name, tok, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
            next();
            if (tok_kind == 4 && strcmp(tok, "(") == 0) {
                Func *f = find_func(name);
                if (!f || f->nparams < 1 || f->ptype[0].kind != 2)
                    die("unknown method");
                if (t.kind != 2 || t.sidx != f->ptype[0].sidx)
                    die("method receiver type mismatch");
                push_value();     /* receiver address */
                next();           /* '(' */
                call_func(f, 1);
                expect(")");
                *is_lv = 0;
                return f->ret;
            }
            if (t.kind != 2)
                die("member access on non-struct");
            int fi = find_field(t.sidx, name);
            if (fi < 0)
                die("unknown field");
            emit(10);             /* CONST_ALT */
            emit((uint32_t)structs[t.sidx].fields[fi].off);
            emit(44);             /* ADD */
            t = structs[t.sidx].fields[fi].type;
        } else if (accept("[")) {
            if (t.kind != 3)
                die("index on non-array");
            push_value();         /* base address */
            expr();
            emit(10);             /* CONST_ALT elem size */
            emit((uint32_t)t.elem_size);
            emit(42);             /* SMUL */
            emit(26);             /* POP_ALT: base */
            emit(44);             /* ADD */
            expect("]");
            Type e;
            memset(&e, 0, sizeof e);
            e.kind = t.elem_kind;
            e.size = t.elem_size;
            e.sidx = t.elem_sidx;
            t = e;
        } else {
            return t;
        }
    }
}

static void emit_native_str(const char *name)
{
    int ni = native_slot(name);
    emit(24);                     /* PUSHR_PRI: PRI holds an AMX address */
    emit(9); emit(4);
    emit(22);
    emit(69); emit((uint32_t)ni);
    emit(28); emit(8);
}

static void emit_print_str(const char *s, int len)
{
    int off = string_slot(s, len);
    emit(9); emit((uint32_t)off);
    emit_native_str("tos_prints");
}

static void emit_print_int(void)
{
    int ni = native_slot("tos_putint");
    emit(22);                     /* PUSH_PRI: value */
    emit(9); emit(4);
    emit(22);
    emit(69); emit((uint32_t)ni);
    emit(28); emit(8);
}

/* True when the next token starts something that is a string value without
 * needing a full expression parse: a literal, a string variable, a char[]
 * variable, or a call to a function returning string. */
/* Leaves the string address/handle in PRI and checks that the expression
 * really produced a string.  A string variable yields its handle, a char[]
 * yields its address, and a literal yields its data offset. */
static void string_arg(void)
{
    expr();
    if (last_kind != 5 && last_kind != 3)
        die("string expression expected");
}

static int is_string_builtin(const char *name)
{
    return strcmp(name, "strlen") == 0 || strcmp(name, "strcmp") == 0 ||
           strcmp(name, "strlcpy") == 0 || strcmp(name, "strlcat") == 0 ||
           strcmp(name, "atoi") == 0;
}

static int builtin_nargs(const char *name)
{
    if (strcmp(name, "strcmp") == 0)
        return 2;
    if (strcmp(name, "strlcpy") == 0 || strcmp(name, "strlcat") == 0)
        return 3;
    return 1;
}

static const char *builtin_native(const char *name)
{
    if (strcmp(name, "strlen") == 0)
        return "tos_strlen";
    if (strcmp(name, "strcmp") == 0)
        return "tos_strcmp";
    if (strcmp(name, "strlcpy") == 0)
        return "tos_strlcpy";
    if (strcmp(name, "strlcat") == 0)
        return "tos_strlcat";
    return "tos_atoi";
}

typedef struct {
    LexState start, end;
} ArgSpan;

static void collect_args(ArgSpan *args, int n)
{
    for (int i = 0; i < n; i++) {
        lex_save(&args[i].start);
        int depth = 0;
        while (tok_kind != 0) {
            if (tok_kind == 4) {
                if (strcmp(tok, "(") == 0 || strcmp(tok, "[") == 0)
                    depth++;
                else if (strcmp(tok, ")") == 0 || strcmp(tok, "]") == 0) {
                    if (depth == 0)
                        break;
                    depth--;
                } else if (strcmp(tok, ",") == 0 && depth == 0) {
                    break;
                }
            }
            next();
        }
        lex_save(&args[i].end);
        if (i + 1 < n) {
            if (!(tok_kind == 4 && strcmp(tok, ",") == 0))
                die("too few arguments");
            next();
        } else {
            if (!(tok_kind == 4 && strcmp(tok, ")") == 0))
                die("too many arguments");
        }
    }
}

/* Assumes the name was consumed; parses "(...)" and emits the native call. */
static void call_string_builtin(const char *name)
{
    int n = builtin_nargs(name);
    ArgSpan args[3];

    expect("(");
    collect_args(args, n);
    for (int i = n - 1; i >= 0; i--) {
        int is_str;
        if (strcmp(name, "strlen") == 0 || strcmp(name, "atoi") == 0)
            is_str = 1;
        else if (strcmp(name, "strcmp") == 0)
            is_str = 1;
        else
            is_str = (i != 1);        /* strlcpy/strlcat: cap is the int */
        lex_restore(&args[i].start);
        if (is_str) {
            string_arg();
            emit(24);                 /* PUSHR_PRI */
        } else {
            expr();
            emit(22);                 /* PUSH_PRI */
        }
    }
    lex_restore(&args[n - 1].end);
    expect(")");

    int ni = native_slot(builtin_native(name));
    emit(9); emit((uint32_t)(n * 4));
    emit(22);
    emit(69); emit((uint32_t)ni);
    emit(28); emit((uint32_t)((n + 1) * 4));
    last_kind = 0;
}

static int is_hw_builtin(const char *name)
{
    return strcmp(name, "gpio_count") == 0 || strcmp(name, "gpio_mode") == 0 ||
           strcmp(name, "gpio_write") == 0 || strcmp(name, "gpio_read") == 0 ||
           strcmp(name, "adc_count") == 0 || strcmp(name, "adc_read") == 0 ||
           strcmp(name, "adc_vref_mv") == 0 || strcmp(name, "adc_bits") == 0 ||
           strcmp(name, "pwm_count") == 0 || strcmp(name, "pwm_set") == 0 ||
           strcmp(name, "pwm_off") == 0;
}

static int hw_nargs(const char *name)
{
    if (strcmp(name, "gpio_count") == 0 || strcmp(name, "adc_count") == 0 ||
        strcmp(name, "adc_vref_mv") == 0 || strcmp(name, "adc_bits") == 0 ||
        strcmp(name, "pwm_count") == 0)
        return 0;
    if (strcmp(name, "gpio_read") == 0 || strcmp(name, "adc_read") == 0 ||
        strcmp(name, "pwm_off") == 0)
        return 1;
    if (strcmp(name, "gpio_mode") == 0 || strcmp(name, "gpio_write") == 0)
        return 2;
    return 3;                         /* pwm_set */
}

static void call_hw_builtin(const char *name)
{
    int n = hw_nargs(name);

    expect("(");
    if (n == 0) {
        expect(")");
    } else {
        ArgSpan args[3];
        collect_args(args, n);
        for (int i = n - 1; i >= 0; i--) {
            lex_restore(&args[i].start);
            expr();
            emit(22);                 /* PUSH_PRI */
        }
        lex_restore(&args[n - 1].end);
        expect(")");
    }
    int ni = native_slot(name);
    emit(9); emit((uint32_t)(n * 4));
    emit(22);
    emit(69); emit((uint32_t)ni);
    emit(28); emit((uint32_t)((n + 1) * 4));
    last_kind = 0;
}

typedef enum {
    A_INT,
    A_STR,
    A_BUF
} ArgKind;

typedef struct {
    const char *name;             /* source-level name */
    const char *native;           /* pvm native name */
    int nargs;
    ArgKind arg[4];
} SysSig;

static const SysSig sys_sigs[] = {
    { "open",          "tos_open",          1, { A_STR } },
    { "create",        "tos_create",        1, { A_STR } },
    { "read",          "tos_read",          3, { A_INT, A_BUF, A_INT } },
    { "write",         "tos_write",         3, { A_INT, A_STR, A_INT } },
    { "write_all",     "tos_write_all",     3, { A_INT, A_STR, A_INT } },
    { "close",         "tos_close",         1, { A_INT } },
    { "lseek",         "tos_lseek",         3, { A_INT, A_INT, A_INT } },
    { "unlink",        "tos_unlink",        1, { A_STR } },
    { "chdir",         "tos_chdir",         1, { A_STR } },
    { "getcwd",        "tos_getcwd",        2, { A_BUF, A_INT } },
    { "readdir_index", "tos_readdir_index", 3, { A_INT, A_BUF, A_INT } },
    { "readdir_path",  "tos_readdir_path",  4, { A_STR, A_INT, A_BUF, A_INT } },
    { "sleep",         "tos_sleep",         1, { A_INT } },
    { "uptime",        "tos_uptime",        0, { A_INT } },
    { "getpid",        "tos_getpid",        0, { A_INT } },
    { "yield",         "tos_yield",         0, { A_INT } },
    { "dup",           "tos_dup",           1, { A_INT } },
    { "ioctl",         "tos_ioctl",         3, { A_INT, A_INT, A_INT } },
    { NULL, NULL, 0, { A_INT } }
};

static const SysSig *find_sys_sig(const char *name)
{
    for (int i = 0; sys_sigs[i].name; i++)
        if (strcmp(sys_sigs[i].name, name) == 0)
            return &sys_sigs[i];
    return NULL;
}

static int is_sys_builtin(const char *name)
{
    return find_sys_sig(name) != NULL;
}

static void call_sys_builtin(const SysSig *sig)
{
    int n = sig->nargs;

    expect("(");
    if (n == 0) {
        expect(")");
    } else {
        ArgSpan args[4];
        collect_args(args, n);
        for (int i = n - 1; i >= 0; i--) {
            lex_restore(&args[i].start);
            switch (sig->arg[i]) {
            case A_INT:
                expr();
                if (last_kind == 6)
                    emit_fix16_toint();
                else if (last_kind != 0)
                    die("integer argument expected");
                emit(22);             /* PUSH_PRI */
                break;
            case A_STR:
                string_arg();
                emit(24);             /* PUSHR_PRI */
                break;
            case A_BUF:
                expr();
                if (last_kind != 3)
                    die("char[] buffer expected");
                emit(24);             /* PUSHR_PRI */
                break;
            }
        }
        lex_restore(&args[n - 1].end);
        expect(")");
    }

    int ni = native_slot(sig->native);
    emit(9); emit((uint32_t)(n * 4));
    emit(22);
    emit(69); emit((uint32_t)ni);
    emit(28); emit((uint32_t)((n + 1) * 4));
    last_kind = 0;
}

static int is_builtin(const char *name)
{
    return is_string_builtin(name) || is_hw_builtin(name) ||
           is_sys_builtin(name);
}

static void call_builtin(const char *name)
{
    if (is_string_builtin(name))
        call_string_builtin(name);
    else if (is_hw_builtin(name))
        call_hw_builtin(name);
    else
        call_sys_builtin(find_sys_sig(name));
}

static void print_stmt(void)
{
    expect("(");
    expr();
    if (last_kind == 5 || last_kind == 3)
        emit_native_str("tos_prints");
    else if (last_kind == 6)
        emit_native_1_pri("tos_fix16_print");
    else
        emit_print_int();
    expect(")");
    expect(";");
}

static void printf_stmt(void)
{
    char fmt[STR_MAX + 1];
    int flen, i;
    char seg[STR_MAX + 1];
    int slen = 0;

    expect("(");
    if (tok_kind != 3)
        die("printf requires a string literal");
    flen = tok_strlen;
    if (flen > STR_MAX)
        flen = STR_MAX;
    memcpy(fmt, tok_str, (size_t)flen);
    fmt[flen] = 0;
    next();

    for (i = 0; i < flen; i++) {
        char ch = fmt[i];
        if (ch != '%') {
            if (slen < STR_MAX)
                seg[slen++] = ch;
            continue;
        }
        if (slen) {
            emit_print_str(seg, slen);
            slen = 0;
        }
        i++;
        if (i >= flen)
            die("printf: trailing %");
        ch = fmt[i];
        if (ch == '%') {
            if (slen < STR_MAX)
                seg[slen++] = '%';
        } else if (ch == 'd' || ch == 'i' || ch == 'u') {
            expect(",");
            expr();
            if (last_kind == 6)
                emit_fix16_toint();
            emit_print_int();
        } else if (ch == 'f') {
            expect(",");
            expr();
            if (last_kind == 0)
                emit_shift_pri(16);
            else if (last_kind != 6)
                die("printf %f needs a numeric value");
            emit_native_1_pri("tos_fix16_print");
        } else if (ch == '.') {
            int dec = 0;
            i++;
            while (i < flen && fmt[i] >= '0' && fmt[i] <= '9') {
                dec = dec * 10 + (fmt[i] - '0');
                i++;
            }
            if (i >= flen || fmt[i] != 'f')
                die("printf: expected f after precision");
            expect(",");
            expr();
            if (last_kind == 0)
                emit_shift_pri(16);
            else if (last_kind != 6)
                die("printf %f needs a numeric value");
            emit_native_2_float_int("tos_fix16_printn", dec);
        } else if (ch == 's') {
            expect(",");
            string_arg();
            emit_native_str("tos_prints");
        } else {
            die("printf: unsupported conversion");
        }
    }
    if (slen)
        emit_print_str(seg, slen);
    expect(")");
    expect(";");
}

static void stmt(void);
static void unary(void);

static void block(void)
{
    expect("{");
    while (tok_kind != 0 && !(tok_kind == 4 && strcmp(tok, "}") == 0))
        stmt();
    expect("}");
}

static void loop_enter(void)
{
    loop_depth++;
    break_depth++;
}

static void loop_patch_cont(int target)
{
    int w = 0;
    for (int i = 0; i < ncont; i++) {
        if (cont_depth[i] == loop_depth)
            patch(cont_fix[i], target);
        else {
            cont_fix[w] = cont_fix[i];
            cont_depth[w] = cont_depth[i];
            w++;
        }
    }
    ncont = w;
}

static void break_patch(int target)
{
    int w = 0;
    for (int i = 0; i < nbrk; i++) {
        if (brk_depth[i] == break_depth)
            patch(brk_fix[i], target);
        else {
            brk_fix[w] = brk_fix[i];
            brk_depth[w] = brk_depth[i];
            w++;
        }
    }
    nbrk = w;
}

static void loop_patch_brk(int target)
{
    break_patch(target);
}

static void loop_leave(void)
{
    loop_depth--;
    break_depth--;
}

static int is_assign_op(void)
{
    if (tok_kind != 4)
        return 0;
    return strcmp(tok, "=") == 0 || strcmp(tok, "+=") == 0 ||
           strcmp(tok, "-=") == 0 || strcmp(tok, "*=") == 0 ||
           strcmp(tok, "/=") == 0 || strcmp(tok, "%=") == 0 ||
           strcmp(tok, "&=") == 0 || strcmp(tok, "|=") == 0 ||
           strcmp(tok, "^=") == 0 || strcmp(tok, "<<=") == 0 ||
           strcmp(tok, ">>=") == 0;
}

/* PRI holds the target address on entry; RHS is still in the token stream. */
static void assign_compound(int kind, Type t)
{
    int is_char = (t.kind == 4);
    if (t.kind == 5 || t.kind == 2 || t.kind == 3)
        die("compound assignment needs a scalar");
    push_value();                 /* address */
    if (is_char) {
        emit(8); emit(1);         /* LODB_I: current byte */
    } else {
        emit(7);                  /* LOAD_I: current cell */
    }
    push_value();                 /* current value */
    expr();                       /* PRI = right-hand side */
    emit(26);                     /* POP_ALT: ALT = current */
    if (t.kind == 6) {
        if (last_kind == 0)
            emit_shift_pri(16);
        else if (last_kind != 6)
            die("numeric value expected");
        switch (kind) {
        case 0:  emit(44); break;                     /* += */
        case 1:  emit(45); break;                     /* -= */
        case 2:  emit_fix16_binop("tos_fix16_mul"); break; /* *= */
        case 3:  emit_fix16_binop("tos_fix16_div"); break; /* /= */
        default: die("float compound op not supported");
        }
        last_kind = 6;
    } else {
        if (last_kind == 6) {
            emit_fix16_toint();
            last_kind = 0;
        } else if (last_kind != 0) {
            die("numeric value expected");
        }
        switch (kind) {
        case 0:  emit(44); break;     /* += */
        case 1:  emit(45); break;     /* -= */
        case 2:  emit(42); break;     /* *= */
        case 3:  emit(43); break;     /* /= */
        case 4:  emit(43); emit(21); break; /* %=: SDIV remainder is in ALT */
        case 5:  emit(46); break;     /* &= */
        case 6:  emit(47); break;     /* |= */
        case 7:  emit(48); break;     /* ^= */
        case 8:  emit(21); emit(37); break; /* <<= */
        case 9:  emit(21); emit(38); break; /* >>= */
        }
        last_kind = 0;
    }
    emit(26);                     /* POP_ALT: address */
    if (is_char) {
        emit(17); emit(1);        /* STRB_I */
    } else {
        emit(16);                 /* STOR_I */
    }
}

/* PRI holds an lvalue address; leaves the updated value in PRI. */
static void post_incdec(int opcode, int is_float, int is_char)
{
    push_value();                 /* address */
    if (is_char) {
        emit(8); emit(1);         /* LODB_I */
    } else {
        emit(7);                  /* LOAD_I: old value */
    }
    if (is_float) {
        emit(22);                 /* PUSH_PRI: current */
        emit(9); emit(FIX_ONE);
        emit(26);                 /* POP_ALT: current */
        emit(opcode == 58 ? 44 : 45); /* ADD / SUB one */
    } else {
        emit(opcode);             /* INC_PRI / DEC_PRI */
    }
    emit(26);                     /* POP_ALT: address */
    if (is_char) {
        emit(17); emit(1);        /* STRB_I */
    } else {
        emit(16);                 /* STOR_I */
    }
    last_kind = is_float ? 6 : 0;
}

static void assign_after_designator(int lv, Type t)
{
    if (!lv)
        die("assignment to rvalue");
    if (accept("=")) {
        if (t.kind == 2 || t.kind == 3)
            die("struct/array assignment not supported");
        push_value();             /* address */
        expr();
        if (t.kind == 5) {
            if (last_kind != 5 && last_kind != 3)
                die("string value expected");
        } else {
            convert_numeric(t);
        }
        emit(26);                 /* POP_ALT: address */
        if (t.kind == 4) {
            emit(17);             /* STRB_I */
            emit(1);
        } else {
            emit(16);             /* STOR_I */
        }
    } else if (accept("+=")) {
        assign_compound(0, t);
    } else if (accept("-=")) {
        assign_compound(1, t);
    } else if (accept("*=")) {
        assign_compound(2, t);
    } else if (accept("/=")) {
        assign_compound(3, t);
    } else if (accept("%=")) {
        assign_compound(4, t);
    } else if (accept("&=")) {
        assign_compound(5, t);
    } else if (accept("|=")) {
        assign_compound(6, t);
    } else if (accept("^=")) {
        assign_compound(7, t);
    } else if (accept("<<=")) {
        assign_compound(8, t);
    } else if (accept(">>=")) {
        assign_compound(9, t);
    } else {
        die("assignment operator expected");
    }
}

static void simple_stmt(void)
{
    if (tok_kind == 1 && find_sym(tok)) {
        int lv;
        Type t = designator(&lv);
        if (accept("++")) {
            if (t.kind != 0 && t.kind != 4 && t.kind != 6)
                die("++/-- needs a scalar");
            post_incdec(58, t.kind == 6, t.kind == 4); /* INC_PRI */
        } else if (accept("--")) {
            if (t.kind != 0 && t.kind != 4 && t.kind != 6)
                die("++/-- needs a scalar");
            post_incdec(61, t.kind == 6, t.kind == 4); /* DEC_PRI */
        } else {
            assign_after_designator(lv, t);
        }
    } else {
        expr();
    }
}

static int cur_slots;

static void emit_return(void)
{
    if (cur_slots) {
        emit(28);                     /* STACK +slots: deallocate locals */
        emit((uint32_t)(cur_slots * 4));
    }
    emit(31);                         /* RET */
}

static void switch_stmt(void)
{
    int case_val[16], case_fix[16], case_used[16];
    int ncase = 0, has_default = 0, def_fix = -1;
    LexState body;

    next();                           /* switch */
    expect("(");
    expr();
    expect(")");
    emit(21);                         /* XCHG: ALT = selector */
    expect("{");
    lex_save(&body);

    /* Pass 1: collect case values at this brace level. */
    {
        int depth = 0;
        while (tok_kind != 0) {
            if (tok_kind == 4 && strcmp(tok, "{") == 0) {
                depth++;
            } else if (tok_kind == 4 && strcmp(tok, "}") == 0) {
                if (depth == 0)
                    break;
                depth--;
            } else if (depth == 0 && tok_kind == 1 &&
                       strcmp(tok, "case") == 0) {
                next();
                int v = parse_const();
                if (tok_kind == 4 && strcmp(tok, ":") == 0 &&
                    ncase < 16) {
                    case_val[ncase] = v;
                    case_fix[ncase] = -1;
                    case_used[ncase] = 0;
                    ncase++;
                }
            } else if (depth == 0 && tok_kind == 1 &&
                       strcmp(tok, "default") == 0) {
                next();
                if (tok_kind == 4 && strcmp(tok, ":") == 0)
                    has_default = 1;
            }
            next();
        }
    }
    lex_restore(&body);

    /* Emit the comparison chain. */
    for (int i = 0; i < ncase; i++) {
        emit(9); emit((uint32_t)case_val[i]);
        emit(52);                     /* EQ: case value == ALT */
        case_fix[i] = emit_jump(36);  /* JNZ body */
    }
    def_fix = emit_jump(34);          /* JUMP default/end */

    break_depth++;
    while (tok_kind != 0 && !(tok_kind == 4 && strcmp(tok, "}") == 0)) {
        if (tok_kind == 1 && strcmp(tok, "case") == 0) {
            next();
            int v = parse_const();
            expect(":");
            int found = -1;
            for (int i = 0; i < ncase; i++)
                if (!case_used[i] && case_val[i] == v) {
                    found = i;
                    break;
                }
            if (found < 0)
                die("case value not pre-scanned");
            patch(case_fix[found], label());
            case_used[found] = 1;
        } else if (tok_kind == 1 && strcmp(tok, "default") == 0) {
            next();
            expect(":");
            patch(def_fix, label());
            def_fix = -1;
            has_default = 1;
        } else {
            stmt();
        }
    }
    expect("}");
    {
        int end = label();
        if (def_fix >= 0)
            patch(def_fix, end);
        for (int i = 0; i < ncase; i++)
            if (!case_used[i])
                patch(case_fix[i], end);
        break_patch(end);
    }
    break_depth--;
    (void)has_default;
}

static void stmt(void)
{
    if (tok_kind == 4 && strcmp(tok, "{") == 0) {
        block();
    } else if (tok_kind == 1 && strcmp(tok, "if") == 0) {
        next();
        expect("(");
        expr();
        expect(")");
        int jz = emit_jump(35);
        stmt();
        if (tok_kind == 1 && strcmp(tok, "else") == 0) {
            int jmp = emit_jump(34);
            patch(jz, label());
            next();
            stmt();
            patch(jmp, label());
        } else {
            patch(jz, label());
        }
    } else if (tok_kind == 1 && strcmp(tok, "while") == 0) {
        next();
        int start = label();
        expect("(");
        expr();
        expect(")");
        int jz = emit_jump(35);
        loop_enter();
        stmt();
        int jmp = emit_jump(34);
        patch(jmp, start);
        int end = label();
        patch(jz, end);
        loop_patch_cont(start);
        loop_patch_brk(end);
        loop_leave();
    } else if (tok_kind == 1 && strcmp(tok, "do") == 0) {
        next();
        loop_enter();
        int body = label();
        stmt();
        if (!(tok_kind == 1 && strcmp(tok, "while") == 0))
            die("while expected");
        next();
        int cond = label();
        loop_patch_cont(cond);
        expect("(");
        expr();
        expect(")");
        expect(";");
        int jnz = emit_jump(36);
        patch(jnz, body);
        int end = label();
        loop_patch_brk(end);
        loop_leave();
    } else if (tok_kind == 1 && strcmp(tok, "switch") == 0) {
        switch_stmt();
    } else if (tok_kind == 1 && strcmp(tok, "for") == 0) {
        next();
        expect("(");
        LexState st;
        lex_save(&st);
        Sym *range = NULL;
        if (tok_kind == 1)
            range = find_sym(tok);
        if (range) {
            next();
            if (tok_kind == 1 && strcmp(tok, "in") == 0) {
                next();
                gen_addr(range);
                emit(22);                 /* PUSH_PRI: address */
                expr();
                emit(26);                 /* POP_ALT: address */
                emit(16);                 /* STOR_I */
                expect("..");
                int start = label();
                gen_addr(range);
                emit(7);                  /* LOAD_I */
                emit(22);
                expr();
                expect(")");
                emit(26);
                emit(56);                 /* SGRTR: end > var */
                int jz = emit_jump(35);
                loop_enter();
                stmt();
                int step = label();
                loop_patch_cont(step);
                gen_addr(range);
                emit(22);                 /* PUSH_PRI: address */
                gen_addr(range);
                emit(7);                  /* LOAD_I: value */
                emit(10); emit(1);        /* CONST_ALT 1 */
                emit(44);                 /* ADD */
                emit(26);                 /* POP_ALT: address */
                emit(16);                 /* STOR_I */
                int jmp = emit_jump(34);
                patch(jmp, start);
                int end = label();
                patch(jz, end);
                loop_patch_brk(end);
                loop_leave();
                return;
            }
        }
        lex_restore(&st);
        if (!(tok_kind == 4 && strcmp(tok, ";") == 0))
            simple_stmt();
        expect(";");
        int start = label();
        int jz = 0;
        if (!(tok_kind == 4 && strcmp(tok, ";") == 0)) {
            expr();
            jz = emit_jump(35);
        }
        expect(";");
        LexState step_state;
        int has_step = 0;
        if (!(tok_kind == 4 && strcmp(tok, ")") == 0)) {
            lex_save(&step_state);
            has_step = 1;
            while (tok_kind != 0 && !(tok_kind == 4 && strcmp(tok, ")") == 0))
                next();
        }
        expect(")");
        loop_enter();
        stmt();
        LexState after;
        lex_save(&after);
        int step = label();
        loop_patch_cont(step);
        if (has_step) {
            lex_restore(&step_state);
            simple_stmt();
            lex_restore(&after);
        }
        int jmp = emit_jump(34);
        patch(jmp, start);
        int end = label();
        if (jz)
            patch(jz, end);
        loop_patch_brk(end);
        loop_leave();
    } else if (tok_kind == 1 && strcmp(tok, "break") == 0) {
        next();
        expect(";");
        if (break_depth == 0)
            die("break outside loop/switch");
        int f = emit_jump(34);
        brk_fix[nbrk] = f;
        brk_depth[nbrk] = break_depth;
        nbrk++;
    } else if (tok_kind == 1 && strcmp(tok, "continue") == 0) {
        next();
        expect(";");
        if (loop_depth == 0)
            die("continue outside loop");
        int f = emit_jump(34);
        cont_fix[ncont] = f;
        cont_depth[ncont] = loop_depth;
        ncont++;
    } else if (tok_kind == 1 && strcmp(tok, "return") == 0) {
        next();
        if (accept(";")) {
            emit(9); emit(0);
            emit_return();
        } else {
            expr();
            if (cur_func->ret.kind == 6 || cur_func->ret.kind == 0 ||
                cur_func->ret.kind == 4)
                convert_numeric(cur_func->ret);
            expect(";");
            emit_return();
        }
    } else if (tok_kind == 1 && strcmp(tok, "print") == 0) {
        next();
        print_stmt();
    } else if (tok_kind == 4 && (strcmp(tok, "++") == 0 ||
                                 strcmp(tok, "--") == 0)) {
        unary();
        expect(";");
    } else if (tok_kind == 1 && strcmp(tok, "printf") == 0) {
        next();
        printf_stmt();
    } else if (tok_kind == 1) {
        if (is_builtin(tok)) {
            LexState save;
            char name[32];
            strncpy(name, tok, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
            lex_save(&save);
            next();
            if (tok_kind == 4 && strcmp(tok, "(") == 0) {
                call_builtin(name);
                expect(";");
                return;
            }
            lex_restore(&save);
        }
        Func *f = find_func(tok);
        if (f && (src_pos < src_len)) {
            LexState save;
            lex_save(&save);
            next();
            if (tok_kind == 4 && strcmp(tok, "(") == 0) {
                next();
                call_func(f, 0);
                expect(")");
                expect(";");
                return;
            }
            lex_restore(&save);
        }
        int lv;
        Type t = designator(&lv);
        if (accept("++")) {
            if (t.kind != 0 && t.kind != 4 && t.kind != 6)
                die("++/-- needs a scalar");
            post_incdec(58, t.kind == 6, t.kind == 4); /* INC_PRI */
            expect(";");
        } else if (accept("--")) {
            if (t.kind != 0 && t.kind != 4 && t.kind != 6)
                die("++/-- needs a scalar");
            post_incdec(61, t.kind == 6, t.kind == 4); /* DEC_PRI */
            expect(";");
        } else if (is_assign_op()) {
            assign_after_designator(lv, t);
            expect(";");
        } else if (lv) {
            if (t.kind == 2 || t.kind == 3)
                die("struct/array used as value");
            emit(7);                  /* LOAD_I */
            expect(";");
        } else {
            expect(";");
        }
    } else {
        die("statement expected");
    }
}

/* Expressions: pri/alt + stack in the same style as csc. */
static void expr_or(void);

static void primary(void)
{
    if (tok_kind == 2) {
        emit(9); emit((uint32_t)tok_num);
        next();
        last_kind = 0;
    } else if (tok_kind == 6) {
        emit(9); emit((uint32_t)tok_num);
        next();
        last_kind = 6;
    } else if (tok_kind == 3) {
        int off = string_slot(tok_str, tok_strlen);
        emit(9); emit((uint32_t)off);
        next();
        last_kind = 5;
    } else if (tok_kind == 1) {
        if (is_builtin(tok)) {
            LexState save;
            char name[32];
            strncpy(name, tok, sizeof(name) - 1);
            name[sizeof(name) - 1] = 0;
            lex_save(&save);
            next();
            if (tok_kind == 4 && strcmp(tok, "(") == 0) {
                call_builtin(name);
                return;
            }
            lex_restore(&save);
        }
        Func *f = find_func(tok);
        if (f) {
            LexState save;
            lex_save(&save);
            next();
            if (tok_kind == 4 && strcmp(tok, "(") == 0) {
                next();
                call_func(f, 0);
                expect(")");
                last_kind = f->ret.kind;
                return;
            }
            lex_restore(&save);
        }
        int ci = find_const(tok);
        if (ci >= 0) {
            emit(9); emit((uint32_t)consts[ci].value);
            next();
            last_kind = 0;
            return;
        }
        int lv;
        Type t = designator(&lv);
        if (t.kind == 0) {
            if (lv)
                emit(7);              /* LOAD_I: int cell */
            last_kind = 0;
        } else if (t.kind == 4) {
            if (lv) {
                emit(8);              /* LODB_I */
                emit(1);
            }
            last_kind = 0;
        } else if (t.kind == 5) {
            if (lv)
                emit(7);              /* LOAD_I: string handle */
            last_kind = 5;
        } else if (t.kind == 6) {
            if (lv)
                emit(7);              /* LOAD_I: Q16.16 cell */
            last_kind = 6;
        } else if (t.kind == 3) {
            last_kind = (t.elem_kind == 4) ? 3 : 0;
        } else {
            die("scalar value expected");
        }
    } else if (accept("(")) {
        expr();
        expect(")");
    } else {
        die("expression expected");
    }
}

/* Prefix ++/-- on a scalar lvalue; leaves the new value in PRI. */
static void pre_incdec(int opcode)
{
    int lv;
    Type t = designator(&lv);
    if (!lv || (t.kind != 0 && t.kind != 4 && t.kind != 6))
        die("++/-- needs a scalar lvalue");
    push_value();                 /* address */
    if (t.kind == 4) {
        emit(8); emit(1);         /* LODB_I */
    } else {
        emit(7);                  /* LOAD_I: old value */
    }
    if (t.kind == 6) {
        emit(22);                 /* PUSH_PRI: current */
        emit(9); emit(FIX_ONE);
        emit(26);                 /* POP_ALT: current */
        emit(opcode == 58 ? 44 : 45); /* ADD / SUB one */
    } else {
        emit(opcode);             /* INC_PRI / DEC_PRI */
    }
    emit(26);                     /* POP_ALT: address */
    if (t.kind == 4) {
        emit(17); emit(1);        /* STRB_I */
    } else {
        emit(16);                 /* STOR_I; PRI keeps the new value */
    }
    last_kind = (t.kind == 6) ? 6 : 0;
}

static void unary(void)
{
    if (accept("-")) {
        int k;
        unary();
        k = last_kind;
        emit(50);                     /* NEG */
        last_kind = (k == 6) ? 6 : 0;
    } else if (accept("!")) {
        unary();
        emit(49);                     /* NOT */
        last_kind = 0;
    } else if (accept("~")) {
        unary();
        if (last_kind == 6)
            die("~ needs an integer operand");
        emit(51);                     /* INVERT */
        last_kind = 0;
    } else if (accept("++")) {
        pre_incdec(58);               /* INC_PRI */
    } else if (accept("--")) {
        pre_incdec(61);               /* DEC_PRI */
    } else {
        primary();
    }
}

/* Q16.16 helpers.  A float in csc is one cell holding value*65536; there is
 * no IEEE representation anywhere in the VM. */
static void emit_shift_pri(int n)
{
    emit(40);                         /* SHL_C_PRI */
    emit((uint32_t)n);
}

static void emit_shift_alt(int n)
{
    emit(41);                         /* SHL_C_ALT */
    emit((uint32_t)n);
}

static void emit_native_1_pri(const char *name)
{
    int ni = native_slot(name);
    emit(22);                         /* PUSH_PRI: value */
    emit(9); emit(4);
    emit(22);
    emit(69); emit((uint32_t)ni);
    emit(28); emit(8);
}

/* PRI = float value, arg2 = integer; native(value, arg2). */
static void emit_native_2_float_int(const char *name, int arg2)
{
    int ni = native_slot(name);
    emit(21);                         /* XCHG: ALT = float value */
    emit(9); emit((uint32_t)arg2);
    emit(22);                         /* push arg2 */
    emit(23);                         /* push float value (ALT) */
    emit(9); emit(8);
    emit(22);
    emit(69); emit((uint32_t)ni);
    emit(28); emit(12);
}

static void emit_fix16_toint(void)
{
    emit_native_1_pri("tos_fix16_toint");
}

/* ALT = left, PRI = right; leaves the Q16.16 result in PRI. */
static void emit_fix16_binop(const char *name)
{
    int ni = native_slot(name);
    emit(22);                         /* PUSH_PRI: right */
    emit(23);                         /* PUSH_ALT: left */
    emit(9); emit(8);
    emit(22);
    emit(69); emit((uint32_t)ni);
    emit(28); emit(12);
}

/* Called after POP_ALT, so ALT holds the left value and PRI the right. */
static void promote_binop(int left_kind, int right_kind)
{
    if (left_kind == 6 && right_kind == 6)
        return;
    if (left_kind != 6 && left_kind != 0)
        die("numeric operand expected");
    if (right_kind != 6 && right_kind != 0)
        die("numeric operand expected");
    if (left_kind != 6)
        emit_shift_alt(16);
    if (right_kind != 6)
        emit_shift_pri(16);
}

static void convert_numeric(Type target)
{
    if (target.kind == 6) {
        if (last_kind == 6)
            return;
        if (last_kind == 0) {
            emit_shift_pri(16);
            last_kind = 6;
            return;
        }
        die("numeric value expected");
    }
    if (target.kind == 0 || target.kind == 4) {
        if (last_kind == 6) {
            emit_fix16_toint();
            last_kind = 0;
            return;
        }
        if (last_kind == 0)
            return;
        die("numeric value expected");
    }
}

static void mul_expr(void)
{
    unary();
    int left_kind = last_kind;
    for (;;) {
        if (accept("*")) {
            push_value();
            unary();
            int right_kind = last_kind;
            emit(26);
            if (left_kind == 6 || right_kind == 6) {
                promote_binop(left_kind, right_kind);
                emit_fix16_binop("tos_fix16_mul");
                last_kind = 6;
            } else {
                emit(42);
                last_kind = 0;
            }
            left_kind = last_kind;
        } else if (accept("/")) {
            push_value();
            unary();
            int right_kind = last_kind;
            emit(26);
            if (left_kind == 6 || right_kind == 6) {
                promote_binop(left_kind, right_kind);
                emit_fix16_binop("tos_fix16_div");
                last_kind = 6;
            } else {
                emit(43);
                last_kind = 0;
            }
            left_kind = last_kind;
        } else if (accept("%")) {
            push_value();
            unary();
            int right_kind = last_kind;
            emit(26);
            if (left_kind == 6 || right_kind == 6)
                die("% needs integer operands");
            emit(43); emit(21);
            last_kind = 0;
            left_kind = 0;
        } else {
            break;
        }
    }
}

static void add_expr(void)
{
    mul_expr();
    int left_kind = last_kind;
    for (;;) {
        if (accept("+")) {
            push_value(); mul_expr();
            int right_kind = last_kind;
            emit(26);
            if (left_kind == 6 || right_kind == 6) {
                promote_binop(left_kind, right_kind);
                emit(44);
                last_kind = 6;
            } else {
                emit(44);
                last_kind = 0;
            }
            left_kind = last_kind;
        } else if (accept("-")) {
            push_value(); mul_expr();
            int right_kind = last_kind;
            emit(26);
            if (left_kind == 6 || right_kind == 6) {
                promote_binop(left_kind, right_kind);
                emit(45);
                last_kind = 6;
            } else {
                emit(45);
                last_kind = 0;
            }
            left_kind = last_kind;
        } else if (accept("<<")) {
            push_value(); mul_expr();
            int right_kind = last_kind;
            emit(26);
            if (left_kind == 6 || right_kind == 6)
                die("shift needs integer operands");
            emit(21); emit(37);
            last_kind = 0;
            left_kind = 0;
        } else if (accept(">>")) {
            push_value(); mul_expr();
            int right_kind = last_kind;
            emit(26);
            if (left_kind == 6 || right_kind == 6)
                die("shift needs integer operands");
            emit(21); emit(38);
            last_kind = 0;
            left_kind = 0;
        } else if (accept("&")) {
            push_value(); mul_expr();
            int right_kind = last_kind;
            emit(26);
            if (left_kind == 6 || right_kind == 6)
                die("bitwise op needs integer operands");
            emit(46);
            last_kind = 0;
            left_kind = 0;
        } else if (accept("|")) {
            push_value(); mul_expr();
            int right_kind = last_kind;
            emit(26);
            if (left_kind == 6 || right_kind == 6)
                die("bitwise op needs integer operands");
            emit(47);
            last_kind = 0;
            left_kind = 0;
        } else if (accept("^")) {
            push_value(); mul_expr();
            int right_kind = last_kind;
            emit(26);
            if (left_kind == 6 || right_kind == 6)
                die("bitwise op needs integer operands");
            emit(48);
            last_kind = 0;
            left_kind = 0;
        } else {
            break;
        }
    }
}

static void rel_expr(void)
{
    add_expr();
    int left_kind = last_kind;
    for (;;) {
        int opcode;
        if (accept("<"))
            opcode = 56;
        else if (accept(">"))
            opcode = 54;
        else if (accept("<="))
            opcode = 57;
        else if (accept(">="))
            opcode = 55;
        else if (accept("=="))
            opcode = 52;
        else if (accept("!="))
            opcode = 53;
        else
            break;
        push_value(); add_expr();
        int right_kind = last_kind;
        emit(26);
        if (left_kind == 6 || right_kind == 6)
            promote_binop(left_kind, right_kind);
        emit((uint32_t)opcode);
        last_kind = 0;
        left_kind = 0;
    }
}

static void expr_and(void)
{
    rel_expr();
    for (;;) {
        if (accept("&&")) {
            int jz = emit_jump(35);
            rel_expr();
            int jz2 = emit_jump(35);
            emit(9); emit(1);
            int jmp = emit_jump(34);
            patch(jz, label());
            patch(jz2, label());
            emit(9); emit(0);
            patch(jmp, label());
            last_kind = 0;
        } else {
            break;
        }
    }
}

static void expr_or(void)
{
    expr_and();
    for (;;) {
        if (accept("||")) {
            int jnz = emit_jump(36);
            expr_and();
            int jnz2 = emit_jump(36);
            emit(9); emit(0);
            int jmp = emit_jump(34);
            patch(jnz, label());
            patch(jnz2, label());
            emit(9); emit(1);
            patch(jmp, label());
            last_kind = 0;
        } else {
            break;
        }
    }
}

static void expr(void)
{
    expr_or();
}

static void emit_func(Func *f)
{
    nlocals = 0;
    cur_func = f;
    cur_slots = 0;
    lex_restore(&f->body);
    expect("{");
    f->label = label();
    emit(30);                         /* PROC */

    for (int i = 0; i < f->nparams; i++) {
        int off = 8 + 4 * (f->nparams - 1 - i);
        new_param(f->pname[i], f->ptype[i], off,
                  f->ptype[i].kind == 2 || f->ptype[i].kind == 3);
    }

    int slots = 0;
    while (is_type_start()) {
        Type t = parse_type();
        if (tok_kind != 1)
            die("local name expected");
        char name[32];
        strncpy(name, tok, sizeof(name) - 1);
        name[sizeof(name) - 1] = 0;
        next();
        if (accept("[")) {
            if (tok_kind != 2)
                die("array length expected");
            int n = tok_num;
            next();
            expect("]");
            t = type_array(t, n);
        }
        if (accept("="))
            die("initialize locals by assignment after declaration");
        expect(";");
        new_local(name, t, &slots);
    }
    if (slots) {
        emit(28);                     /* STACK */
        emit((uint32_t)(-4 * slots));
    }
    cur_slots = slots;
    while (tok_kind != 0 && !(tok_kind == 4 && strcmp(tok, "}") == 0))
        stmt();
    emit(9); emit(0);
    emit_return();
    expect("}");
}

static int write_out(int fd, const void *buf, int n, int *total)
{
    int w = write_all(fd, buf, n);
    if (w != n)
        return -1;
    *total += n;
    return 0;
}

int main(int argc, char **argv)
{
    const char *infile, *outfile;
    int fd, n, names_len = 0, name_off[NAT_MAX];

    if (argc < 3) {
        puts("usage: csc input.c output.amx\n");
        return 2;
    }
    infile = argv[1];
    outfile = argv[2];
    fd = open(infile);
    if (fd < 0) {
        puts("csc: cannot open ");
        puts(infile);
        puts("\n");
        return 1;
    }
    src_len = 0;
    while (src_len < (int)sizeof src &&
           (n = read(fd, src + src_len, sizeof src - src_len)) > 0)
        src_len += n;
    close(fd);
    if (src_len <= 0) {
        puts("csc: empty source\n");
        return 1;
    }

    data_len = 4;                     /* offset 0: the empty string */
    scan_top();
    if (main_func < 0)
        die("no main()");

    code_len = 0;
    emit(9); emit(0);                 /* stub: CONST_PRI 0 */
    emit(67); emit(0);                /* HALT 0 */

    for (int i = 0; i < nfuncs; i++)
        emit_func(&funcs[i]);

    for (int i = 0; i < ncall; i++)
        patch(call_operand[i], funcs[call_target[i]].label);

    for (int i = 0; i < nnatives; i++) {
        name_off[i] = names_len;
        names_len += (int)strlen(natives[i]) + 1;
    }

    int publics = 60;
    int natives_off = 60;
    int libraries = natives_off + nnatives * 8;
    int nametable = libraries;
    int name_start = nametable + 2;
    int cod = align4(name_start + names_len);
    int dat = align4(cod + code_len * 4);
    int hea = align4(dat + data_len);
    int stp = hea + 4096;

    uint8_t hdr[60];
    uint8_t ent[8];
    uint8_t nt[2];
    uint8_t cbuf[256];
    uint8_t zbuf[4] = { 0, 0, 0, 0 };
    int written = 0;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = (uint8_t)hea;
    hdr[1] = (uint8_t)(hea >> 8);
    hdr[2] = (uint8_t)(hea >> 16);
    hdr[3] = (uint8_t)(hea >> 24);
    hdr[4] = 0xe0;
    hdr[5] = 0xf1;
    hdr[6] = 11;
    hdr[7] = 11;
    hdr[10] = 8;
#define PUT32(off, v) do { \
    hdr[(off)] = (uint8_t)(v); hdr[(off)+1] = (uint8_t)((v)>>8); \
    hdr[(off)+2] = (uint8_t)((v)>>16); hdr[(off)+3] = (uint8_t)((v)>>24); \
} while (0)
    PUT32(12, cod);
    PUT32(16, dat);
    PUT32(20, hea);
    PUT32(24, stp);
    PUT32(28, (uint32_t)(funcs[main_func].label * 4));
    PUT32(32, publics);
    PUT32(36, natives_off);
    PUT32(40, libraries);
    PUT32(44, libraries);
    PUT32(48, libraries);
    PUT32(52, nametable);
    PUT32(56, libraries);
#undef PUT32

    fd = create(outfile);
    if (fd < 0) {
        puts("csc: cannot create ");
        puts(outfile);
        puts("\n");
        return 1;
    }
#define OUT(buf, len) do { \
    if (write_out(fd, (buf), (len), &written) < 0) { \
        close(fd); \
        puts("csc: write failed\n"); \
        return 1; \
    } \
} while (0)

    OUT(hdr, 60);
    for (int i = 0; i < nnatives; i++) {
        int no = name_start + name_off[i];
        memset(ent, 0, sizeof ent);
        ent[4] = (uint8_t)no;
        ent[5] = (uint8_t)(no >> 8);
        ent[6] = (uint8_t)(no >> 16);
        ent[7] = (uint8_t)(no >> 24);
        OUT(ent, 8);
    }
    {
        int maxlen = 0;
        for (int i = 0; i < nnatives; i++)
            if ((int)strlen(natives[i]) > maxlen)
                maxlen = (int)strlen(natives[i]);
        nt[0] = (uint8_t)maxlen;
        nt[1] = (uint8_t)(maxlen >> 8);
    }
    OUT(nt, 2);
    for (int i = 0; i < nnatives; i++)
        OUT(natives[i], (int)strlen(natives[i]) + 1);
    if (cod > name_start + names_len)
        OUT(zbuf, cod - (name_start + names_len));

    {
        int cn = 0;
        for (int i = 0; i < code_len; i++) {
            uint32_t w = code[i];
            cbuf[cn++] = (uint8_t)w;
            cbuf[cn++] = (uint8_t)(w >> 8);
            cbuf[cn++] = (uint8_t)(w >> 16);
            cbuf[cn++] = (uint8_t)(w >> 24);
            if (cn == (int)sizeof cbuf) {
                OUT(cbuf, cn);
                cn = 0;
            }
        }
        if (cn)
            OUT(cbuf, cn);
    }
    OUT(data, data_len);
    if (hea > dat + data_len)
        OUT(zbuf, hea - (dat + data_len));
    if (written != hea) {
        close(fd);
        puts("csc: write failed\n");
        return 1;
    }
#undef OUT

    close(fd);
    puts("csc: ");
    puts(infile);
    puts(" -> ");
    puts(outfile);
    puts("\n");
    return 0;
}
