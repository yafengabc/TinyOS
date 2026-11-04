/* py_compile.c - the compiler: tokens in, bytecode out.
 *
 * This is a single-pass compiler.  It walks the token stream once and
 * emits instructions as it goes; there is no tree, so there is nothing
 * to hold in memory between the parse and the run.  That is the whole
 * point: the tree used to be the largest thing the interpreter owned,
 * and on a microcontroller the largest thing in memory decides how long
 * a script can be.
 *
 * The compiler is a recursive-descent parser with the same shape the
 * old tree-building parser had - the precedence ladder from `or` down to
 * the primaries - except that every level now emits code instead of
 * building a node.
 *
 * Two things make it more than a mechanical translation:
 *
 *   - Short-circuit evaluation.  `a and b` and `a or b` do not evaluate
 *     both sides.  The compiler emits a test and a jump, and patches the
 *     jump's destination once the second side has been compiled.
 *
 *   - The scoping rule.  A name is assigned where it is already bound,
 *     searching outward, and defined in the current scope otherwise.
 *     Which scope that is cannot be known at compile time - it depends
 *     on what the program did before the assignment ran - so the
 *     bytecode says "store this name" and the VM does the search at run
 *     time, exactly as the old evaluator did.  A compiler that picked
 *     a slot at compile time would change what the language means.
 *
 * The bytecode's shape is described in py_code.h.
 */

#include "py_code.h"
#include "py_lex.h"
#include "py_mem.h"
#include "py_rt.h"
#include "lib.h"

/* The compiler's error state. */
static const char *comp_err_msg;
static uint32_t comp_err_line;

const char *py_compile_error(void) { return comp_err_msg; }
uint32_t py_compile_error_line(void) { return comp_err_line; }

static void set_error(const char *msg, uint32_t line)
{
    if (!comp_err_msg) {
        comp_err_msg = msg;
        comp_err_line = line;
    }
}

/* ---- the compiler ------------------------------------------------------- */

/* A jump that has not been told its destination yet.  The compiler
 * collects them and patches them when the destination is known. */
typedef struct {
    uint32_t site;        /* the instruction that holds the jump */
    uint32_t label;       /* which label it is waiting for */
} py_jump;

/* A label is a place in the instruction stream.  It is referred to
 * before it exists (a loop's exit is known only after the body has
 * been compiled), so labels are numbered and resolved at the end. */
#define PY_MAX_LABELS 256
#define PY_MAX_JUMPS 512

typedef struct {
    py_prog *prog;
    py_tokens *toks;

    /* The instruction stream being built.  A function definition
     * compiles its body into a stream of its own, so the compiler's
     * state is per function and the definitions nest. */
    uint8_t *ins;
    uint32_t nins, cins;
    uint16_t *lines;
    uint32_t nlines, clines;

    /* Labels: an index into the instruction stream. */
    uint32_t label_pos[PY_MAX_LABELS + 1];
    uint8_t label_set[PY_MAX_LABELS + 1];
    uint32_t nlabels;

    /* Jumps waiting for a label. */
    py_jump jumps[PY_MAX_JUMPS];
    uint32_t njumps;

    /* The indexes of the builtins in the name table, in the order the
     * builtins are declared. */
    uint32_t builtin_names[32];
    uint32_t nbuiltin_names;

    /* The loop stack, for `break` and `continue`. */
    struct {
        uint32_t break_label;      /* where `break` goes */
        uint32_t continue_label;   /* where `continue` goes */
        uint8_t has_iter;           /* does the loop have an iterator to pop? */
    } loops[16];
    uint32_t nloops;
} py_comp;

/* The code object the compiler is filling in.  A function gets one of
 * these; the module gets one too. */
static py_code *new_code(const char *name)
{
    py_code *c = (py_code *)py_alloc((uint32_t)sizeof(py_code));
    if (!c)
        return 0;
    memset(c, 0, sizeof(py_code));
    c->name = name;
    return c;
}

/* Make room for one more code object.
 *
 * The module's code object is number 0 and the functions are numbered
 * from 1, so that the number a function gets never has to be fixed up
 * after the fact - the compiler hands the number to `MAKE_FUNC` as it
 * goes.  That means the array has to exist before the first function is
 * compiled, not after: the counter starts at 1 with the module's slot
 * already reserved, and the room for it is made here. */
static int codes_reserve(py_prog *prog, uint32_t line)
{
    if (prog->codes && prog->ncodes < prog->ccodes)
        return 1;
    uint32_t ncap = prog->ccodes ? prog->ccodes * 2 : 16;
    py_code **nc = (py_code **)py_alloc(ncap * (uint32_t)sizeof(py_code *));
    if (!nc) {
        set_error("out of memory", line);
        return 0;
    }
    memset(nc, 0, ncap * sizeof(py_code *));
    if (prog->codes && prog->ncodes)
        memcpy(nc, prog->codes, prog->ncodes * sizeof(py_code *));
    if (prog->codes)
        py_free_auto(prog->codes);
    prog->codes = nc;
    prog->ccodes = ncap;
    return 1;
}

/* Emit one instruction.  Returns its index.
 *
 * The capacity is counted in instructions, not in bytes: the buffer
 * holds four bytes per instruction, and counting the two in different
 * units is how the buffer ends up three quarters too small and writes
 * over whatever the allocator put after it. */
static uint32_t emit(py_comp *cp, uint8_t op, uint8_t a, uint16_t b, uint32_t line)
{
    if (cp->nins == cp->cins) {
        uint32_t ncap = cp->cins ? cp->cins * 2 : 64;
        uint8_t *ni = (uint8_t *)py_alloc(ncap * (uint32_t)sizeof(py_insn));
        if (!ni) {
            set_error("out of memory", line);
            return 0;
        }
        if (cp->nins)
            memcpy(ni, cp->ins, cp->nins * (uint32_t)sizeof(py_insn));
        if (cp->ins)
            py_free_auto(cp->ins);
        cp->ins = ni;
        cp->cins = ncap;
    }
    if (cp->nlines == cp->clines) {
        uint32_t ncap = cp->clines ? cp->clines * 2 : 64;
        uint16_t *nl = (uint16_t *)py_alloc(ncap * (uint32_t)sizeof(uint16_t));
        if (!nl) {
            set_error("out of memory", line);
            return 0;
        }
        if (cp->nlines)
            memcpy(nl, cp->lines, cp->nlines * sizeof(uint16_t));
        if (cp->lines)
            py_free_auto(cp->lines);
        cp->lines = nl;
        cp->clines = ncap;
    }

    uint32_t at = cp->nins++;
    cp->ins[at * 4 + 0] = op;
    cp->ins[at * 4 + 1] = a;
    cp->ins[at * 4 + 2] = (uint8_t)(b & 0xFF);
    cp->ins[at * 4 + 3] = (uint8_t)(b >> 8);
    cp->lines[cp->nlines++] = (uint16_t)line;
    return at;
}

static uint32_t emit0(py_comp *cp, uint8_t op, uint32_t line)
{
    return emit(cp, op, 0, 0, line);
}

static uint32_t emit1(py_comp *cp, uint8_t op, uint8_t a, uint32_t line)
{
    return emit(cp, op, a, 0, line);
}

static uint32_t emit2(py_comp *cp, uint8_t op, uint16_t b, uint32_t line)
{
    return emit(cp, op, 0, b, line);
}

/* A fresh label.  Its position is not known yet. */
static uint32_t new_label(py_comp *cp)
{
    if (cp->nlabels >= PY_MAX_LABELS) {
        set_error("too many labels", 0);
        return 0;
    }
    /* The labels are numbered from 1: 0 is the "no label" answer, and
     * a label that is silently not placed is a jump that goes
     * nowhere. */
    uint32_t l = ++cp->nlabels;
    cp->label_set[l] = 0;
    return l;
}

/* Put a label at the current position. */
static void place_label(py_comp *cp, uint32_t label)
{
    if (!label || label > cp->nlabels)
        return;
    cp->label_pos[label] = cp->nins;
    cp->label_set[label] = 1;
}

/* Emit a jump to a label that is not placed yet.  The jump records the
 * label; the label's position is filled in later. */
static uint32_t emit_jump(py_comp *cp, uint8_t op, uint32_t label, uint32_t line)
{
    uint32_t site = emit(cp, op, 0, 0, line);
    if (cp->njumps >= PY_MAX_JUMPS) {
        set_error("too many jumps", line);
        return site;
    }
    cp->jumps[cp->njumps].site = site;
    cp->jumps[cp->njumps].label = label;
    cp->njumps++;
    return site;
}

/* Fill in every jump whose label is now known.  The jump's operand is
 * the distance in instructions from the jump to the label, minus one:
 * the instruction after the jump is distance zero. */
static void resolve_jumps(py_comp *cp)
{
    for (uint32_t i = 0; i < cp->njumps; i++) {
        uint32_t label = cp->jumps[i].label;
        if (!label || label > cp->nlabels || !cp->label_set[label])
            continue;
        uint32_t site = cp->jumps[i].site;
        int32_t dist = (int32_t)cp->label_pos[label] - (int32_t)site - 1;
        if (dist < -32768 || dist > 32767) {
            set_error("program too large to compile", 0);
            return;
        }
        cp->ins[site * 4 + 2] = (uint8_t)(dist & 0xFF);
        cp->ins[site * 4 + 3] = (uint8_t)((dist >> 8) & 0xFF);
    }
}

/* ---- names and constants ------------------------------------------------ */

/* A name in the bytecode is an index into the program's name table.
 * The table holds the string once, so a name used a hundred times costs
 * a hundred bytes of index and one string.
 *
 * The strings are owned by the table and live as long as the program
 * does.  The VM looks names up by pointer comparison in the scope
 * hash tables, so the pointer has to be the same one every time - which
 * is exactly what the table guarantees. */
static uint32_t name_index(py_comp *cp, const char *name, uint32_t len)
{
    for (uint32_t i = 0; i < cp->prog->nnames; i++) {
        const char *n = cp->prog->names[i];
        if (strlen(n) == len && !memcmp(n, name, len))
            return i;
    }
    if (cp->prog->nnames == cp->prog->cnames) {
        uint32_t ncap = cp->prog->cnames ? cp->prog->cnames * 2 : 32;
        const char **nn = (const char **)py_alloc(ncap * (uint32_t)sizeof(char *));
        if (!nn) {
            set_error("out of memory", 0);
            return 0;
        }
        if (cp->prog->nnames)
            memcpy(nn, cp->prog->names, cp->prog->nnames * sizeof(char *));
        if (cp->prog->names)
            py_free_auto(cp->prog->names);
        cp->prog->names = nn;
        cp->prog->cnames = ncap;
    }

    char *copy = (char *)py_alloc(len + 1);
    if (!copy) {
        set_error("out of memory", 0);
        return 0;
    }
    memcpy(copy, name, len);
    copy[len] = 0;
    cp->prog->names[cp->prog->nnames++] = copy;
    return cp->prog->nnames - 1;
}

/* A constant in the bytecode is an index into the constant pool.  The
 * pool holds whole values - an int is inline in the value, a string is
 * a heap object the pool owns. */
static uint32_t const_index(py_comp *cp, py_value v)
{
    if (cp->prog->nconsts == cp->prog->cconsts) {
        uint32_t ncap = cp->prog->cconsts ? cp->prog->cconsts * 2 : 32;
        py_value *nc = (py_value *)py_alloc(ncap * (uint32_t)sizeof(py_value));
        if (!nc) {
            set_error("out of memory", 0);
            return 0;
        }
        if (cp->prog->nconsts)
            memcpy(nc, cp->prog->consts, cp->prog->nconsts * sizeof(py_value));
        if (cp->prog->consts)
            py_free_auto(cp->prog->consts);
        cp->prog->consts = nc;
        cp->prog->cconsts = ncap;
    }
    cp->prog->consts[cp->prog->nconsts++] = v;
    return cp->prog->nconsts - 1;
}

/* ---- the token cursor ---------------------------------------------------- */

/* The compiler walks the token array.  Two tokens of lookahead are
 * needed: `x = 1` is an assignment and `x` alone is an expression
 * statement, and the difference is only visible in the token after the
 * name. */
/* The compiler walks the token array.  Two tokens of lookahead are
 * needed: `x = 1` is an assignment and `x` alone is an expression
 * statement, and the difference is only visible in the token after the
 * name.
 *
 * The cursor is an index, not a pointer, so the compiler can look
 * ahead and can compile a range of tokens and then put the cursor back
 * where it was. */
static py_tokens *T;          /* the token stream */
static uint32_t P;            /* the current token index */

static py_token *tk(void) { return P < T->n ? &T->toks[P] : 0; }
static py_token *tk2(void) { return P + 1 < T->n ? &T->toks[P + 1] : 0; }

static void advance(void)
{
    if (P < T->n)
        P++;
}

static int at_op(uint8_t op)
{
    py_token *t = tk();
    return t && t->kind == PY_T_OP && t->op == op;
}

static int at_kw(uint8_t kw)
{
    py_token *t = tk();
    return t && t->kind == PY_T_KEYWORD && t->op == kw;
}

static int at_newline(void)
{
    py_token *t = tk();
    return t && t->kind == PY_T_NEWLINE;
}

static int accept_op(uint8_t op)
{
    if (at_op(op)) {
        advance();
        return 1;
    }
    return 0;
}

static int accept_kw(uint8_t kw)
{
    if (at_kw(kw)) {
        advance();
        return 1;
    }
    return 0;
}

/* ---- expressions --------------------------------------------------------- */

/* Every expression leaves exactly one value on the stack.
 *
 * The precedence ladder, from loosest to tightest:
 *
 *     or
 *     and
 *     not
 *     == != < <= > >= in
 *     + -
 *     * / %
 *     unary - +
 *     f(x)  x[i]  x.attr
 *     literals, names, ( ), [ ], { ]
 *
 * The two boolean operators short-circuit, and Python's `and` and `or`
 * return one of their *operands* rather than a bool - `0 and 1` is 0, and
 * `[] or "x"` is "x".  The old evaluator did that, so the bytecode has
 * to as well.
 *
 * Both are compiled the same way: evaluate the left side, duplicate it,
 * and branch on the copy.  The branch that skips the right side leaves
 * the left value on the stack; the branch that takes the right side
 * drops the left value first.  Either way exactly one value is left,
 * and it is the right one.
 *
 *     <a>                 ; [a]
 *     DUP                 ; [a, a]
 *     JUMP_IF_FALSE L_end ; [a] if false, jump; [a] if true, fall through
 *     POP                 ; []
 *     <b>                 ; [b]
 *   L_end:                ; [a] or [b]
 */

static void compile_or(py_comp *cp);
static void compile_and(py_comp *cp);
static void compile_not(py_comp *cp);
static void compile_comparison(py_comp *cp);
static void compile_arith(py_comp *cp);
static void compile_term(py_comp *cp);
static void compile_unary(py_comp *cp);
static void compile_postfix(py_comp *cp);
static void compile_primary(py_comp *cp);
static void compile_test(py_comp *cp);
static void compile_stmt(py_comp *cp);
static void compile_suite(py_comp *cp);
static void compile_block(py_comp *cp);

/* The `or` level: `a or b or c`, left associative. */
static void compile_or(py_comp *cp)
{
    compile_and(cp);
    while (at_kw(PY_K_OR)) {
        uint32_t line = tk()->line;
        advance();
        uint32_t l_end = new_label(cp);
        emit0(cp, OP_DUP, line);
        emit_jump(cp, OP_JUMP_IF_TRUE, l_end, line);
        emit0(cp, OP_POP, line);
        compile_and(cp);
        place_label(cp, l_end);
    }
}

/* The `and` level. */
static void compile_and(py_comp *cp)
{
    compile_not(cp);
    while (at_kw(PY_K_AND)) {
        uint32_t line = tk()->line;
        advance();
        uint32_t l_end = new_label(cp);
        emit0(cp, OP_DUP, line);
        emit_jump(cp, OP_JUMP_IF_FALSE, l_end, line);
        emit0(cp, OP_POP, line);
        compile_not(cp);
        place_label(cp, l_end);
    }
}

/* `not a`. */
static void compile_not(py_comp *cp)
{
    if (at_kw(PY_K_NOT)) {
        uint32_t line = tk()->line;
        advance();
        compile_not(cp);
        emit0(cp, OP_NOT, line);
        return;
    }
    compile_comparison(cp);
}

/* The comparison level.  A chain of comparisons is left associative, the
 * way the old parser built it. */
static void compile_comparison(py_comp *cp)
{
    compile_arith(cp);
    for (;;) {
        uint8_t op;
        if (at_op(PY_O_EQ))       op = OP_EQ;
        else if (at_op(PY_O_NE))  op = OP_NE;
        else if (at_op(PY_O_LT))  op = OP_LT;
        else if (at_op(PY_O_LE))  op = OP_LE;
        else if (at_op(PY_O_GT))  op = OP_GT;
        else if (at_op(PY_O_GE))  op = OP_GE;
        else if (at_kw(PY_K_IN))  op = OP_IN;
        else return;

        uint32_t line = tk()->line;
        advance();
        compile_arith(cp);
        emit0(cp, op, line);
    }
}

/* The additive level. */
static void compile_arith(py_comp *cp)
{
    compile_term(cp);
    for (;;) {
        uint8_t op;
        if (at_op(PY_O_PLUS))     op = OP_ADD;
        else if (at_op(PY_O_MINUS)) op = OP_SUB;
        else return;

        uint32_t line = tk()->line;
        advance();
        compile_term(cp);
        emit0(cp, op, line);
    }
}

/* The multiplicative level. */
static void compile_term(py_comp *cp)
{
    compile_unary(cp);
    for (;;) {
        uint8_t op;
        if (at_op(PY_O_STAR))      op = OP_MUL;
        else if (at_op(PY_O_SLASH)) op = OP_DIV;
        else if (at_op(PY_O_PERCENT)) op = OP_MOD;
        else return;

        uint32_t line = tk()->line;
        advance();
        compile_unary(cp);
        emit0(cp, op, line);
    }
}

/* Unary minus and plus. */
static void compile_unary(py_comp *cp)
{
    if (at_op(PY_O_MINUS) || at_op(PY_O_PLUS)) {
        int neg = at_op(PY_O_MINUS);
        uint32_t line = tk()->line;
        advance();
        compile_unary(cp);
        if (neg)
            emit0(cp, OP_NEG, line);
        return;
    }
    compile_postfix(cp);
}

/* The postfix level: `f(x)`, `x[i]`, `x.m(y)`.
 *
 * A method call is different from a plain call.  The old evaluator
 * looked the method up by name on the object's type at run time, and
 * there was no such thing as a bound method - `x.append` on its own was
 * an error ("attributes can only be used in a method call").  The
 * bytecode keeps that: OP_CALL_METHOD takes the name, and the VM finds
 * the method in the table for the object's type.
 *
 * The stack for a method call is:
 *
 *     <object>            ; the receiver
 *     <arg0> ... <argN>   ; the arguments
 *
 * so the VM takes the arguments off the top and the receiver below
 * them.  A plain call is:
 *
 *     <callee>
 *     <arg0> ... <argN>
 */
static void compile_postfix(py_comp *cp)
{
    compile_primary(cp);

    for (;;) {
        uint32_t line = tk() ? tk()->line : 0;

        if (at_op(PY_O_LPAREN)) {
            /* A call.  The thing being called is already on the stack. */
            advance();

            /* The arguments.  A keyword argument is `name=value`; the
             * name is known at compile time, so the plan can be built
             * here and the VM can bind it by name. */
            uint8_t npos = 0;
            uint8_t nkw = 0;
            uint32_t plan_at = cp->prog->nplans;
            uint32_t kw_names[16];
            int has_kw = 0;

            if (!at_op(PY_O_RPAREN)) {
                for (;;) {
                    /* Is this argument a keyword?  A name followed by
                     * '=' that is not '==' - the lexer already told the
                     * two apart. */
                    if (tk() && tk()->kind == PY_T_NAME && tk2() &&
                        tk2()->kind == PY_T_OP && tk2()->op == PY_O_ASSIGN) {
                        uint32_t nm = name_index(cp, tk()->text, tk()->len);
                        advance();          /* the name */
                        advance();          /* the '=' */
                        compile_test(cp);
                        if (nkw < 16)
                            kw_names[nkw++] = (uint16_t)nm;
                        has_kw = 1;
                    } else {
                        compile_test(cp);
                        npos++;
                    }
                    if (!accept_op(PY_O_COMMA))
                        break;
                }
            }
            if (!accept_op(PY_O_RPAREN)) {
                set_error("expected ')'", tk() ? tk()->line : line);
                return;
            }

            if (has_kw) {
                /* The plan says, for each argument in order, whether it
                 * is positional or a keyword and, if it is a keyword,
                 * which name it has.  The VM needs both. */
                uint32_t need = 1 + npos + nkw;
                if (cp->prog->nplans + need > cp->prog->cplans) {
                    uint32_t ncap = cp->prog->cplans ? cp->prog->cplans * 2 : 32;
                    while (ncap < cp->prog->nplans + need)
                        ncap *= 2;
                    uint16_t *np = (uint16_t *)py_alloc(ncap * (uint32_t)sizeof(uint16_t));
                    if (!np) {
                        set_error("out of memory", line);
                        return;
                    }
                    if (cp->prog->nplans)
                        memcpy(np, cp->prog->plans, cp->prog->nplans * sizeof(uint16_t));
                    if (cp->prog->plans)
                        py_free_auto(cp->prog->plans);
                    cp->prog->plans = np;
                    cp->prog->cplans = ncap;
                }
                cp->prog->plans[cp->prog->nplans++] = (uint16_t)npos;
                for (uint8_t i = 0; i < nkw; i++)
                    cp->prog->plans[cp->prog->nplans++] = kw_names[i];
                emit1(cp, OP_CALL_KW, (uint8_t)(npos + nkw), line);
                /* The plan offset is the position of the npos word. */
                cp->ins[(cp->nins - 1) * 4 + 2] = (uint8_t)(plan_at & 0xFF);
                cp->ins[(cp->nins - 1) * 4 + 3] = (uint8_t)((plan_at >> 8) & 0xFF);
            } else {
                emit1(cp, OP_CALL, npos, line);
            }
            continue;
        }

        if (at_op(PY_O_LBRACKET)) {
            advance();
            compile_test(cp);
            if (!accept_op(PY_O_RBRACKET)) {
                set_error("expected ']'", tk() ? tk()->line : line);
                return;
            }
            emit0(cp, OP_GET_ITEM, line);
            continue;
        }

        if (at_op(PY_O_DOT)) {
            advance();
            if (!tk() || tk()->kind != PY_T_NAME) {
                set_error("expected an attribute name", tk() ? tk()->line : line);
                return;
            }
            uint32_t nm = name_index(cp, tk()->text, tk()->len);
            uint32_t nameline = tk()->line;
            advance();

            /* `x.m(...)` is a method call; `x.m` on its own is an error,
             * because this interpreter has no bound methods. */
            if (at_op(PY_O_LPAREN)) {
                /* Rewrite the receiver into a method call.  The receiver
                 * is already on the stack; the arguments go on top of
                 * it. */
                advance();
                uint8_t npos = 0;
                if (!at_op(PY_O_RPAREN)) {
                    for (;;) {
                        compile_test(cp);
                        npos++;
                        if (!accept_op(PY_O_COMMA))
                            break;
                    }
                }
                if (!accept_op(PY_O_RPAREN)) {
                    set_error("expected ')'", tk() ? tk()->line : nameline);
                    return;
                }
                emit1(cp, OP_CALL_METHOD, npos, nameline);
                cp->ins[(cp->nins - 1) * 4 + 2] = (uint8_t)(nm & 0xFF);
                cp->ins[(cp->nins - 1) * 4 + 3] = (uint8_t)((nm >> 8) & 0xFF);
                continue;
            }
            emit2(cp, OP_GET_ATTR, (uint16_t)nm, nameline);
            continue;
        }

        return;
    }
}

/* The primary level: literals, names, and the three bracketed forms. */
static void compile_primary(py_comp *cp)
{
    uint32_t line = tk() ? tk()->line : 0;

    if (!tk()) {
        set_error("unexpected end of input", line);
        return;
    }

    if (tk()->kind == PY_T_INT) {
        uint32_t ci = const_index(cp, py_int(tk()->ival));
        emit2(cp, OP_PUSH_CONST, (uint16_t)ci, line);
        advance();
        return;
    }

    if (tk()->kind == PY_T_STR) {
        /* The constant pool owns the string. */
        py_value s = py_new_strn(tk()->text, tk()->len);
        uint32_t ci = const_index(cp, s);
        emit2(cp, OP_PUSH_CONST, (uint16_t)ci, line);
        advance();
        return;
    }

    if (tk()->kind == PY_T_NAME) {
        uint32_t ni = name_index(cp, tk()->text, tk()->len);
        emit2(cp, OP_LOAD, (uint16_t)ni, line);
        advance();
        return;
    }

    if (tk()->kind == PY_T_KEYWORD) {
        if (tk()->op == PY_K_TRUE) {
            emit0(cp, OP_PUSH_TRUE, line);
            advance();
            return;
        }
        if (tk()->op == PY_K_FALSE) {
            emit0(cp, OP_PUSH_FALSE, line);
            advance();
            return;
        }
        if (tk()->op == PY_K_NONE_LIT) {
            emit0(cp, OP_PUSH_NONE, line);
            advance();
            return;
        }
    }

    if (at_op(PY_O_LPAREN)) {
        /* A parenthesised expression, or a tuple.  The old parser turned
         * `(a)` into the value a and `(a, b)` into a list of two; the
         * bytecode does the same. */
        advance();
        if (at_op(PY_O_RPAREN)) {
            /* `()` - an empty list, as the old parser produced. */
            advance();
            emit1(cp, OP_MAKE_LIST, 0, line);
            return;
        }
        compile_test(cp);
        if (at_op(PY_O_COMMA)) {
            /* More than one element: a list. */
            uint16_t n = 1;
            while (accept_op(PY_O_COMMA)) {
                if (at_op(PY_O_RPAREN))
                    break;
                compile_test(cp);
                n++;
            }
            if (!accept_op(PY_O_RPAREN)) {
                set_error("expected ')'", tk() ? tk()->line : line);
                return;
            }
            emit1(cp, OP_MAKE_LIST, n, line);
            return;
        }
        if (!accept_op(PY_O_RPAREN)) {
            set_error("expected ')'", tk() ? tk()->line : line);
            return;
        }
        return;
    }

    if (at_op(PY_O_LBRACKET)) {
        /* A list literal. */
        advance();
        if (at_op(PY_O_RBRACKET)) {
            advance();
            emit1(cp, OP_MAKE_LIST, 0, line);
            return;
        }
        uint16_t n = 0;
        for (;;) {
            compile_test(cp);
            n++;
            if (!accept_op(PY_O_COMMA))
                break;
            if (at_op(PY_O_RBRACKET))
                break;
        }
        if (!accept_op(PY_O_RBRACKET)) {
            set_error("expected ']'", tk() ? tk()->line : line);
            return;
        }
        emit1(cp, OP_MAKE_LIST, n, line);
        return;
    }

    if (at_op(PY_O_LBRACE)) {
        /* A dict literal.  The keys and the values go on the stack
         * alternately, and OP_MAKE_DICT pairs them up. */
        advance();
        if (at_op(PY_O_RBRACE)) {
            advance();
            emit1(cp, OP_MAKE_DICT, 0, line);
            return;
        }
        uint16_t n = 0;
        for (;;) {
            compile_test(cp);       /* the key */
            if (!accept_op(PY_O_COLON)) {
                set_error("expected ':' in dict literal", tk() ? tk()->line : line);
                return;
            }
            compile_test(cp);       /* the value */
            n++;
            if (!accept_op(PY_O_COMMA))
                break;
            if (at_op(PY_O_RBRACE))
                break;
        }
        if (!accept_op(PY_O_RBRACE)) {
            set_error("expected '}'", tk() ? tk()->line : line);
            return;
        }
        emit1(cp, OP_MAKE_DICT, n, line);
        return;
    }

    set_error("unexpected token", line);
}

/* ---- finding a conditional expression ------------------------------------- */

/* A conditional expression is `a if c else b`.  Python evaluates the
 * condition first and then only the branch that is taken, but the
 * source writes the "then" branch first.  The compiler therefore finds
 * the three parts before it emits anything, and then compiles them in
 * the order Python evaluates them: the condition, the "then" branch,
 * the "else" branch.
 *
 * The scan looks ahead in the token stream for an `if` that belongs to
 * this expression - one that is not inside brackets, and not part of
 * another conditional expression.  When it finds the `if`, it finds
 * the `else` that matches it.
 */
typedef struct {
    uint32_t then_start, then_end;     /* the `a` */
    uint32_t cond_start, cond_end;     /* the `c` */
    uint32_t else_start, else_end;     /* the `b` */
} py_cond;

/* The end of an expression: the token after it.  A comma or a closing
 * bracket at this level ends it, and so does the end of the line. */
static uint32_t expr_end(py_tokens *toks, uint32_t from)
{
    uint32_t depth = 0;
    for (uint32_t i = from; i < toks->n; i++) {
        py_token *t = &toks->toks[i];
        if (t->kind == PY_T_NEWLINE && depth == 0)
            return i;
        if (t->kind != PY_T_OP)
            continue;
        if (t->op == PY_O_LPAREN || t->op == PY_O_LBRACKET || t->op == PY_O_LBRACE) {
            depth++;
        } else if (t->op == PY_O_RPAREN || t->op == PY_O_RBRACKET || t->op == PY_O_RBRACE) {
            if (depth == 0)
                return i;
            depth--;
        } else if (t->op == PY_O_COMMA && depth == 0) {
            return i;
        }
    }
    return toks->n;
}

/* Scan for a conditional expression at this level.  Returns 1 and fills
 * `out` when it finds one, 0 when it does not. */
static int scan_conditional(py_tokens *toks, uint32_t from, py_cond *out)
{
    uint32_t depth = 0;
    uint32_t pending = 0;      /* the `if`s still waiting for their `else` */
    uint32_t if_pos = 0;

    for (uint32_t i = from; i < toks->n; i++) {
        py_token *t = &toks->toks[i];

        /* A conditional expression does not cross a line in this
         * subset, so the scan stops at the end of the line.  Without
         * that, a dictionary literal would be scanned past its own
         * closing brace and the `if` of the next statement would look
         * like a conditional expression. */
        if (t->kind == PY_T_NEWLINE && depth == 0)
            return 0;

        if (t->kind == PY_T_OP) {
            if (t->op == PY_O_LPAREN || t->op == PY_O_LBRACKET || t->op == PY_O_LBRACE) {
                depth++;
            } else if (t->op == PY_O_RPAREN || t->op == PY_O_RBRACKET || t->op == PY_O_RBRACE) {
                if (depth == 0)
                    return 0;        /* the expression ended before any `if` */
                depth--;
                /* The bracket that closed the last group ends the
                 * expression too.  Without this, a dictionary literal
                 * would be scanned past its own closing brace, and the
                 * `if` of the next statement would look like a
                 * conditional expression. */
                if (depth == 0)
                    return 0;
            } else if (t->op == PY_O_COMMA && depth == 0) {
                return 0;
            } else if (t->op == PY_O_COLON && depth == 0) {
                return 0;            /* a dict key, or the end of a header */
            } else if (t->op == PY_O_ASSIGN && depth == 0) {
                return 0;            /* the end of an assignment target */
            }
            continue;
        }

        if (t->kind != PY_T_KEYWORD)
            continue;

        if (t->op == PY_K_IF) {
            if (depth != 0)
                continue;
            if (pending == 0) {
                if_pos = i;
            }
            pending++;
            continue;
        }

        if (t->op == PY_K_ELSE) {
            if (depth != 0 || pending == 0)
                continue;
            pending--;
            if (pending != 0)
                continue;            /* this `else` belongs to a nested one */

            /* This is the `else` of the outermost `if` we saw, which is
             * the one that belongs to this expression. */
            out->then_start = from;
            out->then_end = if_pos;
            out->cond_start = if_pos + 1;
            out->cond_end = i;
            out->else_start = i + 1;
            out->else_end = expr_end(toks, i + 1);
            return 1;
        }
    }
    return 0;
}

/* Compile the tokens in [from, to) as an expression.  The cursor is
 * left at `to`. */
static void compile_range(py_comp *cp, py_tokens *toks, uint32_t from, uint32_t to);

/* A test is an expression that can be used as a condition.  The only
 * thing above `or` is the conditional expression. */
static void compile_test(py_comp *cp)
{
    py_tokens *toks = cp->toks;
    uint32_t start = P;
    uint32_t line = tk() ? tk()->line : 0;

    py_cond cond;
    if (!scan_conditional(toks, start, &cond)) {
        compile_or(cp);
        return;
    }


    /* The condition comes first in the order Python evaluates things,
     * even though it comes second in the source. */
    uint32_t l_else = new_label(cp);
    uint32_t l_end = new_label(cp);

    compile_range(cp, toks, cond.cond_start, cond.cond_end);
    emit_jump(cp, OP_JUMP_IF_FALSE, l_else, line);

    compile_range(cp, toks, cond.then_start, cond.then_end);
    emit_jump(cp, OP_JUMP, l_end, line);

    place_label(cp, l_else);
    compile_range(cp, toks, cond.else_start, cond.else_end);
    place_label(cp, l_end);
}

/* ---- statements ---------------------------------------------------------- */

/* Compile the body of an `if`, a loop or a function: either a suite on
 * the same line, or an indented block.  The lexer has already turned the
 * indentation into INDENT and DEDENT tokens, so this is the same shape
 * the old parser had. */
static void compile_suite(py_comp *cp)
{
    uint32_t line = tk() ? tk()->line : 0;

    if (!accept_op(PY_O_COLON)) {
        set_error("expected ':'", tk() ? tk()->line : line);
        return;
    }

    /* A suite on the same line: `if x: return 1`. */
    if (!at_newline()) {
        compile_stmt(cp);
        return;
    }

    advance();                    /* the NEWLINE */

    py_token *t = tk();
    if (!t || t->kind != PY_T_INDENT) {
        set_error("expected an indented block", t ? t->line : line);
        return;
    }
    advance();                    /* the INDENT */

    while (tk() && tk()->kind != PY_T_DEDENT && tk()->kind != PY_T_EOF)
        compile_stmt(cp);

    t = tk();
    if (!t || t->kind != PY_T_DEDENT) {
        set_error("expected an indented block", t ? t->line : line);
        return;
    }
    advance();                    /* the DEDENT */
}

/* A simple statement ends with the newline that terminates its line.
 * Consuming it here is what lets an `elif` follow an `if` block: without
 * it the newline would be taken for a statement of its own and the block
 * would end one line early. */
static void end_statement(void)
{
    if (at_newline())
        advance();
}

/* An `if` statement, with the `elif` and `else` arms. */
static void compile_if(py_comp *cp)
{
    uint32_t line = tk() ? tk()->line : 0;
    advance();                    /* the `if` */

    uint32_t l_else = new_label(cp);
    compile_test(cp);
    emit_jump(cp, OP_JUMP_IF_FALSE, l_else, line);

    compile_suite(cp);

    if (at_kw(PY_K_ELIF)) {
        /* `elif` is an `if` without a block of its own: it becomes a
         * nested `if` in the else arm. */
        uint32_t l_end = new_label(cp);
        emit_jump(cp, OP_JUMP, l_end, line);
        place_label(cp, l_else);
        compile_if(cp);
        place_label(cp, l_end);
        return;
    }

    if (at_kw(PY_K_ELSE)) {
        uint32_t l_end = new_label(cp);
        emit_jump(cp, OP_JUMP, l_end, line);
        place_label(cp, l_else);
        advance();                /* the `else` */
        compile_suite(cp);
        place_label(cp, l_end);
        return;
    }

    place_label(cp, l_else);
}

/* A `while` loop.
 *
 * The old evaluator ran the condition, ran the body, and looped.  The
 * bytecode does the same: the condition is at the top, and the jump
 * back to it is at the bottom.  `break` jumps to the end, `continue`
 * jumps to the condition. */
static void compile_while(py_comp *cp)
{
    uint32_t line = tk() ? tk()->line : 0;
    advance();                    /* the `while` */

    uint32_t l_top = new_label(cp);
    uint32_t l_end = new_label(cp);
    place_label(cp, l_top);

    compile_test(cp);
    emit_jump(cp, OP_JUMP_IF_FALSE, l_end, line);

    /* The loop context, so that `break` and `continue` know where to
     * go. */
    if (cp->nloops >= 16) {
        set_error("too many nested loops", line);
        return;
    }
    cp->loops[cp->nloops].break_label = l_end;
    cp->loops[cp->nloops].continue_label = l_top;
    cp->loops[cp->nloops].has_iter = 0;
    cp->nloops++;

    compile_suite(cp);
    cp->nloops--;

    emit_jump(cp, OP_JUMP, l_top, line);
    place_label(cp, l_end);
}

/* A `for` loop.
 *
 * The old evaluator turned the iterable into a list of items and walked
 * that.  The bytecode does the same: OP_FOR_PREP turns the iterable
 * into a list on the VM's stack, and OP_FOR_NEXT takes the next item
 * off it and jumps to the end when there are none left.
 *
 * The loop variable is assigned, not defined - the old evaluator used
 * the same "assign where the name is bound" rule as any other
 * assignment, and so does this. */
static void compile_for(py_comp *cp)
{
    uint32_t line = tk() ? tk()->line : 0;
    advance();                    /* the `for` */

    py_token *t = tk();
    if (!t || t->kind != PY_T_NAME) {
        set_error("expected a variable name after 'for'", t ? t->line : line);
        return;
    }
    uint32_t var = name_index(cp, t->text, t->len);
    advance();

    if (!accept_kw(PY_K_IN)) {
        set_error("expected 'in'", tk() ? tk()->line : line);
        return;
    }

    compile_test(cp);             /* the iterable */
    emit0(cp, OP_FOR_PREP, line);
    /* FOR_PREP leaves two things on the stack: the list of items and the
     * index into it, which is where the loop is. */
    emit1(cp, OP_PUSH_INT0, 0, line);

    uint32_t l_top = new_label(cp);
    uint32_t l_end = new_label(cp);
    place_label(cp, l_top);

    emit_jump(cp, OP_FOR_NEXT, l_end, line);
    emit2(cp, OP_STORE, (uint16_t)var, line);

    if (cp->nloops >= 16) {
        set_error("too many nested loops", line);
        return;
    }
    cp->loops[cp->nloops].break_label = l_end;
    cp->loops[cp->nloops].continue_label = l_top;
    cp->loops[cp->nloops].has_iter = 1;
    cp->nloops++;

    compile_suite(cp);
    cp->nloops--;

    emit_jump(cp, OP_JUMP, l_top, line);
    place_label(cp, l_end);

    /* The iterator is on the stack when the loop is left, by every
     * path out of it: the normal end, and `break`. */
    emit0(cp, OP_POP_ITERS, line);
}

/* A function definition.
 *
 * The body is compiled into a code object of its own, so the function
 * has its own instructions, its own parameters and its own line table.
 * The enclosing function gets one instruction - MAKE_FUNC - that pushes
 * a function object referring to it.
 *
 * The function object is created when the definition *runs*, not when
 * it is compiled, so the closure can be captured: the old evaluator
 * did the same, and a function that refers to a name of the scope it
 * was defined in has to see that scope at run time. */
static void compile_def(py_comp *cp)
{
    uint32_t line = tk() ? tk()->line : 0;
    uint32_t def_start = P;
    advance();                    /* the `def` */

    py_token *t = tk();
    if (!t || t->kind != PY_T_NAME) {
        set_error("expected a function name", t ? t->line : line);
        return;
    }
    uint32_t nm = name_index(cp, t->text, t->len);
    advance();

    if (!accept_op(PY_O_LPAREN)) {
        set_error("expected '('", tk() ? tk()->line : line);
        return;
    }

    /* The parameter names, in order. */
    uint16_t params[16];
    uint32_t nparams = 0;
    if (!at_op(PY_O_RPAREN)) {
        for (;;) {
            t = tk();
            if (!t || t->kind != PY_T_NAME) {
                set_error("expected a parameter name", t ? t->line : line);
                return;
            }
            if (nparams >= 16) {
                set_error("too many parameters", t->line);
                return;
            }
            params[nparams++] = (uint16_t)name_index(cp, t->text, t->len);
            advance();
            if (!accept_op(PY_O_COMMA))
                break;
        }
    }
    if (!accept_op(PY_O_RPAREN)) {
        set_error("expected ')'", tk() ? tk()->line : line);
        return;
    }

    /* Compile the body into a code object of its own.  This second
     * compiler state is allocated, not on the stack - see py_compile()
     * for why. */
    py_comp *sub = (py_comp *)py_alloc((uint32_t)sizeof(py_comp));
    if (!sub) {
        set_error("out of memory", line);
        return;
    }
    memset(sub, 0, sizeof(py_comp));
    sub->prog = cp->prog;
    sub->toks = cp->toks;

    /* The parameter names are copied into the code object, so the
     * function knows what to bind. */
    uint16_t *plist = 0;
    if (nparams) {
        plist = (uint16_t *)py_alloc(nparams * (uint32_t)sizeof(uint16_t));
        if (!plist) {
            set_error("out of memory", line);
            py_free_auto(sub);
            return;
        }
        memcpy(plist, params, nparams * sizeof(uint16_t));
    }

    /* The token cursor is shared: the body ends where the sub compiler
     * leaves it, and the cursor is NOT put back afterwards (that would
     * compile the same `def` forever).  What has to be restored is the
     * loop stack - a `break` in the body belongs to a loop in the body,
     * not to a loop around the definition. */
    uint32_t saved_nloops = cp->nloops;

    sub->nlabels = 0;
    sub->njumps = 0;
    memset(sub->label_pos, 0, sizeof(sub->label_pos));
    memset(sub->label_set, 0, sizeof(sub->label_set));
    sub->nloops = 0;

    compile_suite(sub);
    cp->nloops = saved_nloops;

    /* The body has its own label space, so its jumps are resolved on
     * their own. */
    resolve_jumps(sub);

    py_code *fc = (py_code *)py_alloc((uint32_t)sizeof(py_code));
    if (!fc) {
        set_error("out of memory", line);
        py_free_auto(sub);
        return;
    }
    memset(fc, 0, sizeof(py_code));
    fc->ins = sub->ins;
    fc->nins = sub->nins;
    fc->lines = sub->lines;
    fc->params = plist;
    fc->nparams = nparams;
    fc->name = cp->prog->names[nm];

    if (!codes_reserve(cp->prog, line)) {
        py_free_auto(sub);
        return;
    }
    uint32_t code_index = cp->prog->ncodes++;
    cp->prog->codes[code_index] = fc;

    /* And the enclosing function gets the instruction that creates it. */
    emit2(cp, OP_MAKE_FUNC, (uint16_t)code_index, line);
    emit2(cp, OP_DEFINE, (uint16_t)nm, line);
    py_free_auto(sub);
}

/* An assignment.
 *
 * There are two forms.  `x = v` names a variable, and `a[i] = v` names
 * a slot in a container.  The old evaluator handled them with one
 * function that took a target node; the bytecode has two instructions
 * instead, and the compiler picks one after it has seen the target.
 *
 * The value is evaluated first, in both cases - Python evaluates the
 * right hand side before the target. */
/* The bracket that closes the one at `from`.
 *
 * `a[i][j] = v` has brackets inside brackets, so the scan counts the
 * depth and stops at the one that closes the outermost bracket of the
 * subscript. */
static uint32_t match_bracket(py_tokens *toks, uint32_t from)
{
    uint32_t depth = 0;
    for (uint32_t i = from; i < toks->n; i++) {
        py_token *t = &toks->toks[i];
        if (t->kind != PY_T_OP)
            continue;
        if (t->op == PY_O_LBRACKET || t->op == PY_O_LPAREN ||
            t->op == PY_O_LBRACE) {
            depth++;
        } else if (t->op == PY_O_RBRACKET || t->op == PY_O_RPAREN ||
                   t->op == PY_O_RBRACE) {
            depth--;
            if (depth == 0)
                return i;
        }
    }
    return toks->n;
}

static void compile_assign(py_comp *cp, uint32_t line)
{
    /* The target is a name, or a name with a subscript. */
    py_token *t = tk();
    if (!t || t->kind != PY_T_NAME) {
        set_error("cannot assign to this expression", line);
        return;
    }
    uint32_t nm = name_index(cp, t->text, t->len);
    advance();

    if (at_op(PY_O_LBRACKET)) {
        /* `a[i] = v`.  Python evaluates the value first, then the
         * container, then the index - the source writes them in the
         * other order, so the index is compiled out of its own stretch
         * of the token stream, after the value has been compiled. */
        uint32_t lbracket = P;
        uint32_t rbracket = match_bracket(cp->toks, lbracket);

        /* The value, which in the source comes after the `=`. */
        P = rbracket + 1;
        if (!accept_op(PY_O_ASSIGN)) {
            set_error("expected '='", tk() ? tk()->line : line);
            return;
        }
        compile_test(cp);
        uint32_t after = P;

        /* The container, then the index. */
        emit2(cp, OP_LOAD, (uint16_t)nm, line);
        compile_range(cp, cp->toks, lbracket + 1, rbracket);
        P = after;

        emit0(cp, OP_SET_ITEM, line);
        return;
    }

    if (!accept_op(PY_O_ASSIGN)) {
        set_error("expected '='", tk() ? tk()->line : line);
        return;
    }
    compile_test(cp);
    emit2(cp, OP_STORE, (uint16_t)nm, line);
}

/* Is this statement an assignment?
 *
 * The target can be a name (`x = 1`) or a name with a subscript
 * (`a[i] = 1`), so the `=` is not always the token after the name.
 * The scan looks for it at the top level of the statement - not inside
 * brackets, and not after a comparison operator, which is not an
 * assignment in Python either.
 *
 * It stops at the end of the line, because a statement ends there. */
static int stmt_is_assign(py_comp *cp)
{
    py_tokens *toks = cp->toks;
    uint32_t depth = 0;

    for (uint32_t i = P; i < toks->n; i++) {
        py_token *t = &toks->toks[i];
        if (t->kind == PY_T_NEWLINE)
            return 0;
        if (t->kind == PY_T_OP) {
            if (t->op == PY_O_LPAREN || t->op == PY_O_LBRACKET ||
                t->op == PY_O_LBRACE) {
                depth++;
            } else if (t->op == PY_O_RPAREN || t->op == PY_O_RBRACKET ||
                       t->op == PY_O_RBRACE) {
                if (depth == 0)
                    return 0;
                depth--;
            } else if (depth == 0) {
                if (t->op == PY_O_ASSIGN)
                    return 1;
                if (t->op == PY_O_PLUSEQ || t->op == PY_O_MINUSEQ ||
                    t->op == PY_O_STAREQ || t->op == PY_O_SLASHEQ ||
                    t->op == PY_O_PERCENTEQ)
                    return 2;
            }
        }
    }
    return 0;
}

/* One statement: a compound statement (if, while, for, def) or a simple
 * one. */
static void compile_stmt(py_comp *cp)
{
    uint32_t line = tk() ? tk()->line : 0;

    if (!tk()) {
        set_error("unexpected end of input", line);
        return;
    }

    /* A stray blank line: skip it. */
    if (at_newline()) {
        advance();
        return;
    }

    if (tk()->kind == PY_T_KEYWORD) {
        switch (tk()->op) {
        case PY_K_IF:
            compile_if(cp);
            return;
        case PY_K_WHILE:
            compile_while(cp);
            return;
        case PY_K_FOR:
            compile_for(cp);
            return;
        case PY_K_DEF:
            compile_def(cp);
            return;

        case PY_K_RETURN: {
            advance();
            /* A bare return gives None.  The value is optional only
             * when the line ends here. */
            if (!at_newline() && tk()->kind != PY_T_DEDENT &&
                tk()->kind != PY_T_EOF) {
                compile_test(cp);
            } else {
                emit0(cp, OP_PUSH_NONE, line);
            }
            emit0(cp, OP_RETURN, line);
            end_statement();
            return;
        }

        case PY_K_BREAK: {
            advance();
            if (!cp->nloops) {
                set_error("'break' outside a loop", line);
                return;
            }
            uint32_t l_end = cp->loops[cp->nloops - 1].break_label;
            uint32_t l_cont = cp->loops[cp->nloops - 1].continue_label;
            uint8_t has_iter = cp->loops[cp->nloops - 1].has_iter;
            /* Leaving a for loop early means the iterator is still on
             * the stack, so it has to be dropped before the jump. */
            if (has_iter)
                emit0(cp, OP_POP_ITERS, line);
            (void)l_cont;
            emit_jump(cp, OP_JUMP, l_end, line);
            end_statement();
            return;
        }

        case PY_K_CONTINUE: {
            advance();
            if (!cp->nloops) {
                set_error("'continue' outside a loop", line);
                return;
            }
            uint32_t l_cont = cp->loops[cp->nloops - 1].continue_label;
            emit_jump(cp, OP_JUMP, l_cont, line);
            end_statement();
            return;
        }

        case PY_K_PASS:
            advance();
            end_statement();
            return;

        case PY_K_DEL: {
            advance();
            py_token *t = tk();
            if (!t || t->kind != PY_T_NAME) {
                set_error("cannot delete this expression", line);
                return;
            }
            uint32_t nm = name_index(cp, t->text, t->len);
            advance();
            if (at_op(PY_O_LBRACKET)) {
                /* `del a[i]`: the container, then the index. */
                advance();
                emit2(cp, OP_LOAD, (uint16_t)nm, line);
                compile_test(cp);
                if (!accept_op(PY_O_RBRACKET)) {
                    set_error("expected ']'", tk() ? tk()->line : line);
                    return;
                }
                emit0(cp, OP_DEL_ITEM, line);
            } else {
                emit2(cp, OP_DELETE, (uint16_t)nm, line);
            }
            end_statement();
            return;
        }

        default:
            break;
        }
    }

    /* An assignment starts with a name.  Whether it is one is not
     * visible from the token after the name - `a[i] = 1` has a bracket
     * there - so the statement is scanned for its `=`. */
    if (tk()->kind == PY_T_NAME) {
        int kind = stmt_is_assign(cp);

        if (kind == 1) {
            compile_assign(cp, line);
            end_statement();
            return;
        }

        /* An augmented assignment: `x += 1`. */
        if (kind == 2) {
            /* The target has to be a plain name: the old evaluator
             * refused `a[i] += 1` the same way. */
            if (!tk() || tk()->kind != PY_T_NAME || !tk2() ||
                tk2()->kind != PY_T_OP) {
                set_error("cannot assign to this expression", line);
                return;
            }
            uint32_t nm = name_index(cp, tk()->text, tk()->len);
            uint8_t binop;
            switch (tk2()->op) {
            case PY_O_PLUSEQ:    binop = OP_ADD; break;
            case PY_O_MINUSEQ:   binop = OP_SUB; break;
            case PY_O_STAREQ:    binop = OP_MUL; break;
            case PY_O_SLASHEQ:   binop = OP_DIV; break;
            default:             binop = OP_MOD; break;
            }
            advance();                    /* the name */
            advance();                    /* the operator */
            /* Python reads the name's current value before it works
             * out the right hand side, so the value comes second and
             * the operator sees (old value, new value) - which is what
             * makes `x -= 1` subtract from x and not the other way
             * round. */
            emit2(cp, OP_LOAD, (uint16_t)nm, line);
            compile_test(cp);
            emit0(cp, binop, line);
            emit2(cp, OP_STORE, (uint16_t)nm, line);
            end_statement();
            return;
        }
    }

    /* An expression statement.  Its value is dropped. */
    compile_test(cp);
    emit0(cp, OP_POP, line);
    end_statement();
}

/* Compile the tokens in [from, to) as an expression.  The cursor is
 * left at `to`.
 *
 * This is how a conditional expression is put together: the condition
 * is compiled first, even though the source writes it second, so each
 * part is compiled from its own stretch of the token stream. */
static void compile_range(py_comp *cp, py_tokens *toks, uint32_t from, uint32_t to)
{
    uint32_t saved = P;
    P = from;
    compile_test(cp);
    P = to;
}

/* The top level: compile the module. */
py_prog *py_compile(py_tokens *toks)
{
    comp_err_msg = 0;
    comp_err_line = 0;

    if (!toks || !toks->toks) {
        set_error("empty program", 0);
        return 0;
    }

    py_prog *prog = (py_prog *)py_alloc((uint32_t)sizeof(py_prog));
    if (!prog) {
        set_error("out of memory", 0);
        return 0;
    }
    memset(prog, 0, sizeof(py_prog));
    prog->toks = toks;

    /* The compiler state holds the label table and the jump patch list:
     * over 5 KiB.  Two of them are live while a `def` body is compiled,
     * so keeping them on the stack cost 11 KiB before the expression
     * recursion even started - more than the whole 16 KiB stack on the
     * ESP32-C3.  It lives in the arena instead; py_alloc()'s oversize
     * list hands the very same block to the next `def`. */
    py_comp *cp = (py_comp *)py_alloc((uint32_t)sizeof(py_comp));
    if (!cp) {
        set_error("out of memory", 0);
        return 0;
    }
    memset(cp, 0, sizeof(py_comp));
    cp->prog = prog;
    cp->toks = toks;
    py_code *mc = 0;

    /* The module's own code is code object 0, so the functions can be
     * numbered from the start and the numbering does not have to be
     * fixed up afterwards. */
    prog->ncodes = 1;
    if (!codes_reserve(prog, 0))
        goto fail;

    T = toks;
    P = 0;

    /* The module's own code. */
    while (P < T->n && tk()->kind != PY_T_EOF) {
        if (tk()->kind == PY_T_NEWLINE) {
            advance();
            continue;
        }
        if (tk()->kind == PY_T_DEDENT) {
            /* A dedent with no block open: the file ended. */
            advance();
            continue;
        }
        compile_stmt(cp);
        if (comp_err_msg)
            goto fail;
    }

    resolve_jumps(cp);

    /* The builtins have to be in the name table too: the module scope
     * holds them under the names the table has, so that a script's own
     * `print` is the same name and not a copy of it. */
    {
        uint32_t n = py_builtins_count();
        for (uint32_t i = 0; i < n; i++) {
            const char *nm = py_builtins[i].name;
            uint32_t len = (uint32_t)strlen(nm);
            uint32_t idx = name_index(cp, nm, len);
            if (cp->nbuiltin_names < 32)
                cp->builtin_names[cp->nbuiltin_names++] = idx;
        }
    }

    /* The module's code object. */
    mc = (py_code *)py_alloc((uint32_t)sizeof(py_code));
    if (!mc) {
        set_error("out of memory", 0);
        goto fail;
    }
    memset(mc, 0, sizeof(py_code));
    mc->ins = cp->ins;
    mc->nins = cp->nins;
    mc->lines = cp->lines;
    mc->name = "<module>";

    /* The module's code object goes into the slot that was reserved for
     * it; the functions keep the numbers they were given. */
    if (!codes_reserve(prog, 0))
        goto fail;
    prog->codes[0] = mc;

    /* The builtins, in the name table. */
    for (uint32_t i = 0; i < cp->nbuiltin_names && i < 32; i++)
        prog->builtin_names[prog->nbuiltin_names++] = (uint16_t)cp->builtin_names[i];

    py_free_auto(cp);
    return prog;

fail:
    py_free_auto(cp);
    return 0;
}

uint32_t py_prog_insns(py_prog *prog)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < prog->ncodes; i++)
        n += prog->codes[i]->nins;
    return n;
}

uint32_t py_prog_bytes(py_prog *prog)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < prog->ncodes; i++)
        n += prog->codes[i]->nins * 4;
    return n;
}
