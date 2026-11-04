/* py_parse.c - the parser.
 *
 * Recursive descent over the token list.  Python's grammar is small enough
 * that a table-driven parser would not pay for itself: the whole language
 * here is expressions, assignments and four kinds of block.
 *
 * The precedence ladder, from loosest to tightest:
 *
 *   or
 *   and
 *   not
 *   == != < <= > >= in
 *   + -
 *   * / %
 *   unary - +
 *   f(x)  x[i]  x.attr
 *   literals, names, ( ), [ ], { }
 *
 * Statements:
 *
 *   assignment        x = expr
 *   augmented         x += expr
 *   expression        expr
 *   if / elif / else
 *   while
 *   for x in expr
 *   def f(a, b):
 *   return / break / continue / pass
 */

#include "py_parse.h"
#include "py_str.h"
#include "py_mem.h"

/* The token cursor. */
static py_token *tk;
static py_token *tk_next;
/* The parser's own error state.  A syntax error can also come from the
 * lexer (a bad indent, an unterminated string), so the public accessors
 * fold the two together: whichever reported first wins. */
static uint32_t err_line;
static const char *err_msg;

const char *py_err_msg(void)
{
    return err_msg ? err_msg : py_lex_err_msg();
}

uint32_t py_err_line(void)
{
    return err_msg ? err_line : py_lex_err_line();
}

static void set_error(const char *msg, uint32_t line)
{
    if (!err_msg) {
        err_msg = msg;
        err_line = line;
    }
}

static py_token *peek(void) { return tk; }
static py_token *peek2(void) { return tk_next; }

/* Move to the next token.  tk_next is the token after tk, so that the
 * parser can look two ahead - which is how it tells an assignment
 * (NAME '=') from an expression statement (NAME alone). */
static void advance(void)
{
    if (tk) {
        tk = tk->next;
        tk_next = tk ? tk->next : 0;
    }
}

static int at_op(uint8_t op) { return tk && tk->kind == PY_T_OP && tk->op == op; }
static int at_kw(uint8_t kw) { return tk && tk->kind == PY_T_KEYWORD && tk->op == kw; }
/* NEWLINE and INDENT are token kinds, not operators, so they are asked for
 * by kind rather than through at_op(). */
static int at_newline(void) { return tk && tk->kind == PY_T_NEWLINE; }

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

static py_node *new_node(uint8_t kind, uint32_t line)
{
    /* A tree node is never released, so it takes a permanent block: no
     * header, no rounding.  A hundred lines of script make a few hundred
     * of these, and the headers would be a third of the arena. */
    py_node *n = (py_node *)py_alloc_perm((uint32_t)sizeof(py_node));
    if (!n) {
        /* The arena is full.  Say so: "syntax error" would send whoever
         * is reading the message looking for a comma. */
        set_error("out of memory", line);
        return 0;
    }
    memset(n, 0, sizeof(py_node));
    n->kind = kind;
    n->line = line;
    n->u.sval = py_none();
    return n;
}

/* ---- expressions ------------------------------------------------------ */

static py_node *parse_expr(void);
static py_node *parse_test(void);      /* the 'not' level */
static py_node *parse_or(void);
static py_node *parse_and(void);
static py_node *parse_not(void);
static py_node *parse_comparison(void);
static py_node *parse_arith(void);
static py_node *parse_term(void);
static py_node *parse_unary(void);
static py_node *parse_postfix(void);
static py_node *parse_primary(void);

/* The statement parsers, defined further down. */
static py_node *parse_block(void);
static py_node *parse_suite(void);
static py_node *parse_if(void);
static py_node *parse_while(void);
static py_node *parse_for(void);
static py_node *parse_def(void);

/* Parse a parenthesised expression or a tuple: (a, b, c).  A single
 * element is just the element; more than one is a list of values the
 * caller can unpack. */
static py_node *parse_paren(void)
{
    uint32_t line = tk ? tk->line : 0;
    advance();                    /* '(' */

    py_node *list = new_node(PY_E_GROUP, line);
    if (!list)
        return 0;

    if (at_op(PY_O_RPAREN)) {
        advance();
        return list;
    }

    py_node *first = parse_test();
    if (!first)
        return 0;
    list->a = first;

    while (accept_op(PY_O_COMMA)) {
        if (at_op(PY_O_RPAREN))
            break;
        py_node *e = parse_test();
        if (!e)
            return 0;
        /* Chain the elements through ->next so the list keeps them in
         * order without a second array. */
        py_node *tail = list->a;
        while (tail->next)
            tail = tail->next;
        tail->next = e;
    }

    if (!accept_op(PY_O_RPAREN)) {
        set_error("expected ')'", tk ? tk->line : line);
        return 0;
    }
    return list;
}

static py_node *parse_list_literal(void)
{
    uint32_t line = tk ? tk->line : 0;
    advance();                    /* '[' */

    py_node *list = new_node(PY_E_LIST, line);
    if (!list)
        return 0;

    if (at_op(PY_O_RBRACKET)) {
        advance();
        return list;
    }

    py_node *first = parse_test();
    if (!first)
        return 0;
    list->a = first;

    while (accept_op(PY_O_COMMA)) {
        if (at_op(PY_O_RBRACKET))
            break;
        py_node *e = parse_test();
        if (!e)
            return 0;
        py_node *tail = list->a;
        while (tail->next)
            tail = tail->next;
        tail->next = e;
    }

    if (!accept_op(PY_O_RBRACKET)) {
        set_error("expected ']'", tk ? tk->line : line);
        return 0;
    }
    return list;
}

static py_node *parse_dict_literal(void)
{
    uint32_t line = tk ? tk->line : 0;
    advance();                    /* '{' */

    py_node *d = new_node(PY_E_DICT, line);
    if (!d)
        return 0;

    if (at_op(PY_O_RBRACE)) {
        advance();
        return d;
    }

    /* Keys and values are kept in two parallel chains: the key chain
     * through ->next, the value chain through ->next as well. */
    py_node *k_tail = 0;
    py_node *v_tail = 0;

    for (;;) {
        py_node *k = parse_test();
        if (!k)
            return 0;
        if (!accept_op(PY_O_COLON)) {
            set_error("expected ':' in dict literal", tk ? tk->line : line);
            return 0;
        }
        py_node *v = parse_test();
        if (!v)
            return 0;

        if (!d->u.dict.keys) {
            d->u.dict.keys = k;
            d->u.dict.vals = v;
        } else {
            k_tail->next = k;
            v_tail->next = v;
        }
        k_tail = k;
        v_tail = v;

        if (!accept_op(PY_O_COMMA))
            break;
        if (at_op(PY_O_RBRACE))
            break;
    }

    if (!accept_op(PY_O_RBRACE)) {
        set_error("expected '}'", tk ? tk->line : line);
        return 0;
    }
    return d;
}

static py_node *parse_primary(void)
{
    uint32_t line = tk ? tk->line : 0;

    if (!tk) {
        set_error("unexpected end of input", line);
        return 0;
    }

    if (tk->kind == PY_T_INT) {
        py_node *n = new_node(PY_E_INT, line);
        if (!n)
            return 0;
        n->ival = tk->ival;
        advance();
        return n;
    }
    if (tk->kind == PY_T_STR) {
        py_node *n = new_node(PY_E_STR, line);
        if (!n)
            return 0;
        n->u.sval = py_new_strn(tk->text, (uint32_t)strlen(tk->text));
        advance();
        return n;
    }
    if (tk->kind == PY_T_NAME) {
        py_node *n = new_node(PY_E_NAME, line);
        if (!n)
            return 0;
        n->name = tk->text;
        advance();
        return n;
    }
    if (tk->kind == PY_T_KEYWORD) {
        if (tk->op == PY_K_TRUE || tk->op == PY_K_FALSE) {
            py_node *n = new_node(PY_E_BOOL, line);
            if (!n)
                return 0;
            n->ival = (tk->op == PY_K_TRUE);
            advance();
            return n;
        }
        if (tk->op == PY_K_NONE_LIT) {
            py_node *n = new_node(PY_E_NONE, line);
            if (!n)
                return 0;
            advance();
            return n;
        }
    }
    if (at_op(PY_O_LPAREN))
        return parse_paren();
    if (at_op(PY_O_LBRACKET))
        return parse_list_literal();
    if (at_op(PY_O_LBRACE))
        return parse_dict_literal();

    set_error("unexpected token", line);
    return 0;
}

/* f(x, y) and x[i] and x.attr */
static py_node *parse_postfix(void)
{
    py_node *e = parse_primary();
    if (!e)
        return 0;

    for (;;) {
        uint32_t line = tk ? tk->line : e->line;

        if (at_op(PY_O_LPAREN)) {
            advance();
            py_node *call = new_node(PY_E_CALL, line);
            if (!call)
                return 0;
            call->a = e;

            py_node *tail = 0;
            if (!at_op(PY_O_RPAREN)) {
                for (;;) {
                    /* A keyword argument: name=value. */
                    py_node *arg = parse_test();
                    if (!arg)
                        return 0;
                    if (at_op(PY_O_ASSIGN) && arg->kind == PY_E_NAME) {
                        advance();
                        py_node *val = parse_test();
                        if (!val)
                            return 0;
                        py_node *pair = new_node(PY_E_BINOP, line);
                        if (!pair)
                            return 0;
                        pair->op = PY_OP_NONE;
                        pair->name = arg->name;
                        pair->a = val;
                        arg = pair;
                    }
                    if (!call->u.call.args) {
                        call->u.call.args = arg;
                    } else {
                        tail->next = arg;
                    }
                    tail = arg;
                    if (!accept_op(PY_O_COMMA))
                        break;
                }
            }
            if (!accept_op(PY_O_RPAREN)) {
                set_error("expected ')'", tk ? tk->line : line);
                return 0;
            }
            e = call;
            continue;
        }

        if (at_op(PY_O_LBRACKET)) {
            advance();
            py_node *idx = new_node(PY_E_INDEX, line);
            if (!idx)
                return 0;
            idx->a = e;
            idx->b = parse_test();
            if (!idx->b)
                return 0;
            if (!accept_op(PY_O_RBRACKET)) {
                set_error("expected ']'", tk ? tk->line : line);
                return 0;
            }
            e = idx;
            continue;
        }

        if (at_op(PY_O_DOT)) {
            advance();
            if (!tk || tk->kind != PY_T_NAME) {
                set_error("expected an attribute name", tk ? tk->line : line);
                return 0;
            }
            py_node *attr = new_node(PY_E_ATTR, line);
            if (!attr)
                return 0;
            attr->a = e;
            attr->name = tk->text;
            advance();
            e = attr;
            continue;
        }

        return e;
    }
}

static py_node *parse_unary(void)
{
    uint32_t line = tk ? tk->line : 0;

    if (at_op(PY_O_MINUS)) {
        advance();
        py_node *n = new_node(PY_E_UNARY, line);
        if (!n)
            return 0;
        n->op = PY_OP_SUB;
        n->a = parse_unary();
        return n->a ? n : 0;
    }
    if (at_op(PY_O_PLUS)) {
        advance();
        py_node *n = new_node(PY_E_UNARY, line);
        if (!n)
            return 0;
        n->op = PY_OP_ADD;
        n->a = parse_unary();
        return n->a ? n : 0;
    }
    return parse_postfix();
}

static py_node *parse_term(void)
{
    py_node *e = parse_unary();
    if (!e)
        return 0;

    for (;;) {
        uint8_t op;
        if (at_op(PY_O_STAR))
            op = PY_OP_MUL;
        else if (at_op(PY_O_SLASH))
            op = PY_OP_DIV;
        else if (at_op(PY_O_PERCENT))
            op = PY_OP_MOD;
        else
            return e;

        uint32_t line = tk->line;
        advance();
        py_node *n = new_node(PY_E_BINOP, line);
        if (!n)
            return 0;
        n->op = op;
        n->a = e;
        n->b = parse_unary();
        if (!n->b)
            return 0;
        e = n;
    }
}

static py_node *parse_arith(void)
{
    py_node *e = parse_term();
    if (!e)
        return 0;

    for (;;) {
        uint8_t op;
        if (at_op(PY_O_PLUS))
            op = PY_OP_ADD;
        else if (at_op(PY_O_MINUS))
            op = PY_OP_SUB;
        else
            return e;

        uint32_t line = tk->line;
        advance();
        py_node *n = new_node(PY_E_BINOP, line);
        if (!n)
            return 0;
        n->op = op;
        n->a = e;
        n->b = parse_term();
        if (!n->b)
            return 0;
        e = n;
    }
}

static py_node *parse_comparison(void)
{
    py_node *e = parse_arith();
    if (!e)
        return 0;

    for (;;) {
        uint8_t op;
        if (at_op(PY_O_EQ))
            op = PY_OP_EQ;
        else if (at_op(PY_O_NE))
            op = PY_OP_NE;
        else if (at_op(PY_O_LT))
            op = PY_OP_LT;
        else if (at_op(PY_O_LE))
            op = PY_OP_LE;
        else if (at_op(PY_O_GT))
            op = PY_OP_GT;
        else if (at_op(PY_O_GE))
            op = PY_OP_GE;
        else if (at_kw(PY_K_IN))
            op = PY_OP_IN;
        else
            return e;

        uint32_t line = tk->line;
        advance();
        py_node *n = new_node(PY_E_COMPARE, line);
        if (!n)
            return 0;
        n->op = op;
        n->a = e;
        n->b = parse_arith();
        if (!n->b)
            return 0;
        e = n;
    }
}

static py_node *parse_not(void)
{
    if (at_kw(PY_K_NOT)) {
        uint32_t line = tk->line;
        advance();
        py_node *n = new_node(PY_E_UNARY, line);
        if (!n)
            return 0;
        n->op = PY_OP_NONE;       /* 'not' is marked by op == PY_OP_NONE
                                 * on an unary node; see py_eval.c */
        n->ival = 1;             /* not flag */
        n->a = parse_not();
        return n->a ? n : 0;
    }
    return parse_comparison();
}

static py_node *parse_and(void)
{
    py_node *e = parse_not();
    if (!e)
        return 0;

    while (at_kw(PY_K_AND)) {
        uint32_t line = tk->line;
        advance();
        py_node *n = new_node(PY_E_BOOLOP, line);
        if (!n)
            return 0;
        n->op = PY_OP_AND;
        n->a = e;
        n->b = parse_not();
        if (!n->b)
            return 0;
        e = n;
    }
    return e;
}

static py_node *parse_or(void)
{
    py_node *e = parse_and();
    if (!e)
        return 0;

    while (at_kw(PY_K_OR)) {
        uint32_t line = tk->line;
        advance();
        py_node *n = new_node(PY_E_BOOLOP, line);
        if (!n)
            return 0;
        n->op = PY_OP_OR;
        n->a = e;
        n->b = parse_and();
        if (!n->b)
            return 0;
        e = n;
    }
    return e;
}

static py_node *parse_expr(void) { return parse_or(); }

/* A test is an expression that can be used as a condition: 'or' level
 * with a conditional expression on top.  The conditional expression
 * (a if c else b) is the one Python construct that is not an operator
 * prefix, so it sits above the boolean operators. */
static py_node *parse_test(void)
{
    py_node *e = parse_or();
    if (!e)
        return 0;

    if (at_kw(PY_K_IF)) {
        uint32_t line = tk->line;
        advance();
        py_node *cond = parse_or();
        if (!cond)
            return 0;
        if (!accept_kw(PY_K_ELSE)) {
            set_error("expected 'else' in conditional expression", tk ? tk->line : line);
            return 0;
        }
        py_node *other = parse_test();
        if (!other)
            return 0;

        py_node *n = new_node(PY_E_BINOP, line);
        if (!n)
            return 0;
        n->op = PY_OP_NONE;
        n->ival = 1;             /* marks a conditional expression */
        n->a = e;
        n->b = cond;
        n->c = other;
        return n;
    }
    return e;
}

/* ---- statements ------------------------------------------------------- */

/* Parse an indented suite: a ':' then either a simple statement on the
 * same line, or a NEWLINE and an indented block. */
static py_node *parse_suite(void)
{
    uint32_t line = tk ? tk->line : 0;

    if (!accept_op(PY_O_COLON)) {
        set_error("expected ':'", tk ? tk->line : line);
        return 0;
    }

    /* A suite on the same line: "if x: return 1". */
    if (!at_newline())
        return parse_block();

    advance();                    /* the NEWLINE */

    if (!tk || tk->kind != PY_T_INDENT) {
        set_error("expected an indented block", tk ? tk->line : line);
        return 0;
    }
    advance();                    /* the INDENT */

    py_node *block = new_node(PY_S_BLOCK, line);
    if (!block)
        return 0;

    py_node *tail = 0;
    while (tk && tk->kind != PY_T_DEDENT && tk->kind != PY_T_EOF) {
        py_node *st = parse_block();
        if (!st)
            return 0;
        if (!block->list) {
            block->list = st;
        } else {
            tail->next = st;
        }
        tail = st;
    }

    if (!tk || tk->kind != PY_T_DEDENT) {
        set_error("expected an indented block", tk ? tk->line : line);
        return 0;
    }
    advance();                    /* the DEDENT */
    return block;
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

/* One statement, or a compound statement. */
static py_node *parse_block(void)
{
    uint32_t line = tk ? tk->line : 0;

    if (!tk || tk->kind == PY_T_NEWLINE) {
        /* A stray blank line: skip it. */
        if (tk && tk->kind == PY_T_NEWLINE) {
            advance();
            return new_node(PY_S_PASS, line);
        }
        set_error("unexpected end of input", line);
        return 0;
    }

    if (tk->kind == PY_T_KEYWORD) {
        switch (tk->op) {
        case PY_K_IF:
            return parse_if();
        case PY_K_WHILE:
            return parse_while();
        case PY_K_FOR:
            return parse_for();
        case PY_K_DEF:
            return parse_def();

        case PY_K_RETURN: {
            py_node *n = new_node(PY_S_RETURN, line);
            if (!n)
                return 0;
            advance();                    /* 'return' */
            /* A bare return gives None.  The value is optional only when
             * the line ends here. */
            if (!at_newline() && tk->kind != PY_T_DEDENT && tk->kind != PY_T_EOF) {
                n->a = parse_test();
                if (!n->a)
                    return 0;
            }
            end_statement();
            return n;
        }

        case PY_K_BREAK: {
            py_node *n = new_node(PY_S_BREAK, line);
            if (!n)
                return 0;
            advance();
            end_statement();
            return n;
        }

        case PY_K_CONTINUE: {
            py_node *n = new_node(PY_S_CONTINUE, line);
            if (!n)
                return 0;
            advance();
            end_statement();
            return n;
        }

        case PY_K_PASS: {
            py_node *n = new_node(PY_S_PASS, line);
            if (!n)
                return 0;
            advance();
            end_statement();
            return n;
        }

        case PY_K_DEL: {
            py_node *n = new_node(PY_S_DEL, line);
            if (!n)
                return 0;
            advance();                    /* 'del' */
            n->a = parse_test();
            if (!n->a)
                return 0;
            if (n->a->kind != PY_E_NAME && n->a->kind != PY_E_INDEX) {
                set_error("cannot delete this expression", line);
                return 0;
            }
            end_statement();
            return n;
        }

        default:
            break;
        }
    }

    /* An assignment starts with a name; everything else is an expression
     * statement.  The target is a node like any other, so that the
     * evaluator can handle 'a[i] = v' with the same code. */
    if (tk->kind == PY_T_NAME && peek2() && peek2()->kind == PY_T_OP &&
        peek2()->op == PY_O_ASSIGN) {
        py_node *n = new_node(PY_S_ASSIGN, line);
        if (!n)
            return 0;
        py_node *target = new_node(PY_E_NAME, line);
        if (!target)
            return 0;
        target->name = tk->text;
        n->a = target;
        advance();                /* the name */
        advance();                /* the '=' */
        n->b = parse_test();
        if (!n->b)
            return 0;
        end_statement();
        return n;
    }

    /* Augmented assignment: x += 1 and friends. */
    if (tk->kind == PY_T_NAME && peek2() && peek2()->kind == PY_T_OP) {
        uint8_t op = peek2()->op;
        uint8_t aop;
        switch (op) {
        case PY_O_PLUSEQ:   aop = PY_OP_ADD; break;
        case PY_O_MINUSEQ:  aop = PY_OP_SUB; break;
        case PY_O_STAREQ:   aop = PY_OP_MUL; break;
        case PY_O_SLASHEQ:  aop = PY_OP_DIV; break;
        case PY_O_PERCENTEQ: aop = PY_OP_MOD; break;
        default:            aop = PY_OP_NONE; break;
        }
        if (aop != PY_OP_NONE) {
            py_node *n = new_node(PY_S_AUGASSIGN, line);
            if (!n)
                return 0;
            py_node *target = new_node(PY_E_NAME, line);
            if (!target)
                return 0;
            target->name = tk->text;
            n->a = target;
            n->op = aop;
            advance();
            advance();
            n->b = parse_test();
            if (!n->b)
                return 0;
            end_statement();
            return n;
        }
    }

    /* An expression statement - unless an '=' follows the expression, in
     * which case the expression was a target: a[i] = v is an assignment
     * just as much as x = v is. */
    py_node *expr = parse_test();
    if (!expr)
        return 0;

    if (at_op(PY_O_ASSIGN)) {
        if (expr->kind != PY_E_NAME && expr->kind != PY_E_INDEX &&
            expr->kind != PY_E_ATTR) {
            set_error("cannot assign to this expression", line);
            return 0;
        }
        advance();                    /* the '=' */
        py_node *n = new_node(PY_S_ASSIGN, line);
        if (!n)
            return 0;
        n->a = expr;
        n->b = parse_test();
        if (!n->b)
            return 0;
        end_statement();
        return n;
    }

    py_node *n = new_node(PY_S_EXPR, line);
    if (!n)
        return 0;
    n->a = expr;
    end_statement();
    return n;
}

static py_node *parse_if(void)
{
    uint32_t line = tk->line;
    advance();                    /* 'if' */

    py_node *n = new_node(PY_S_IF, line);
    if (!n)
        return 0;
    n->a = parse_test();
    if (!n->a)
        return 0;
    n->b = parse_suite();
    if (!n->b)
        return 0;

    if (at_kw(PY_K_ELIF)) {
        /* elif is an if without a block of its own: rewrite it as a
         * nested if in the else branch. */
        n->c = parse_if();
        if (!n->c)
            return 0;
        return n;
    }
    if (at_kw(PY_K_ELSE)) {
        advance();
        n->c = parse_suite();
        if (!n->c)
            return 0;
    }
    return n;
}

static py_node *parse_while(void)
{
    uint32_t line = tk->line;
    advance();                    /* 'while' */

    py_node *n = new_node(PY_S_WHILE, line);
    if (!n)
        return 0;
    n->a = parse_test();
    if (!n->a)
        return 0;
    n->b = parse_suite();
    if (!n->b)
        return 0;
    return n;
}

static py_node *parse_for(void)
{
    uint32_t line = tk->line;
    advance();                    /* 'for' */

    if (!tk || tk->kind != PY_T_NAME) {
        set_error("expected a variable name after 'for'", tk ? tk->line : line);
        return 0;
    }
    py_node *n = new_node(PY_S_FOR, line);
    if (!n)
        return 0;
    n->name = tk->text;
    advance();

    if (!accept_kw(PY_K_IN)) {
        set_error("expected 'in'", tk ? tk->line : line);
        return 0;
    }
    n->a = parse_test();
    if (!n->a)
        return 0;
    n->b = parse_suite();
    if (!n->b)
        return 0;
    return n;
}

static py_node *parse_def(void)
{
    uint32_t line = tk->line;
    advance();                    /* 'def' */

    if (!tk || tk->kind != PY_T_NAME) {
        set_error("expected a function name", tk ? tk->line : line);
        return 0;
    }
    py_node *n = new_node(PY_S_FUNC, line);
    if (!n)
        return 0;
    n->name = tk->text;
    advance();

    if (!accept_op(PY_O_LPAREN)) {
        set_error("expected '('", tk ? tk->line : line);
        return 0;
    }

    /* The parameter names, in order. */
    uint32_t cap = 4;
    n->u.fn.params = (char **)py_alloc(cap * (uint32_t)sizeof(char *));
    if (!n->u.fn.params)
        return 0;

    if (!at_op(PY_O_RPAREN)) {
        for (;;) {
            if (!tk || tk->kind != PY_T_NAME) {
                set_error("expected a parameter name", tk ? tk->line : line);
                return 0;
            }
            if (n->u.fn.nparams == cap) {
                uint32_t ncap = cap * 2;
                char **np = (char **)py_alloc(ncap * (uint32_t)sizeof(char *));
                if (!np)
                    return 0;
                memcpy(np, n->u.fn.params, cap * sizeof(char *));
                py_free_auto(n->u.fn.params);
                n->u.fn.params = np;
                cap = ncap;
            }
            n->u.fn.params[n->u.fn.nparams++] = tk->text;
            advance();
            if (!accept_op(PY_O_COMMA))
                break;
        }
    }
    if (!accept_op(PY_O_RPAREN)) {
        set_error("expected ')'", tk ? tk->line : line);
        return 0;
    }

    /* The body goes into a scope of its own: the evaluator gives a call
     * its own environment, and the scope is what says which statements
     * that environment starts with. */
    py_node *suite = parse_suite();
    if (!suite)
        return 0;
    py_scope *sc = (py_scope *)py_alloc((uint32_t)sizeof(py_scope));
    if (!sc)
        return 0;
    memset(sc, 0, sizeof(py_scope));
    sc->body = suite;
    n->u.fn.body = sc;
    return n;
}

/* The top level: statements until EOF. */
static py_scope *parse_program(void)
{
    py_scope *sc = (py_scope *)py_alloc((uint32_t)sizeof(py_scope));
    if (!sc)
        return 0;
    memset(sc, 0, sizeof(py_scope));

    py_node *block = new_node(PY_S_BLOCK, 1);
    if (!block)
        return 0;

    py_node *tail = 0;
    while (tk && tk->kind != PY_T_EOF) {
        if (tk->kind == PY_T_NEWLINE) {
            advance();
            continue;
        }
        if (tk->kind == PY_T_DEDENT) {
            /* A dedent with no block open: the file ended. */
            advance();
            continue;
        }
        py_node *st = parse_block();
        if (!st)
            return 0;
        if (!block->list)
            block->list = st;
        else
            tail->next = st;
        tail = st;
    }

    sc->body = block;
    return sc;
}

py_scope *py_parse(const char *src, uint32_t len)
{
    err_msg = 0;
    err_line = 0;

    py_token *toks = py_lex_all(src, len);
    if (!toks)
        return 0;

    tk = toks;
    tk_next = toks->next;

    py_scope *sc = parse_program();
    if (!sc && !err_msg)
        set_error("syntax error", tk ? tk->line : 1);

    return sc;
}
