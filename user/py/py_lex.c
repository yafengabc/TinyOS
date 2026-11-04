/* py_lex.c - the lexer.
 *
 * Python's blocks are indentation, so the lexer carries an indent stack
 * and emits INDENT and DEDENT the way CPython's tokenizer does:
 *
 *   - the indentation of a line that has code on it is compared against
 *     the top of the stack; a deeper one pushes an INDENT, a shallower one
 *     pops levels and emits a DEDENT for each;
 *   - a blank line, or one holding only a comment, produces no NEWLINE
 *     and changes no indentation - which is why a comment block between
 *     two statements of the same block does not close it;
 *   - inside brackets a newline is not a statement end, so the lexer
 *     keeps reading until the brackets balance.
 *
 * Tabs are rejected rather than guessed at: CPython refuses to mix tabs
 * and spaces, and a program that means one thing on one machine and
 * another on the next is worse than one that is refused.
 */

#include "py_parse.h"
#include "py_str.h"
#include "py_mem.h"

static py_token *tok_head;
static py_token *tok_tail;
static uint32_t cur_line = 1;

/* The indent stack: the column of each open block level. */
static uint32_t indents[32];
static int nindents;
static int paren_depth;

/* Error state.  The lexer has its own, separate from the parser's: a
 * syntax error can come from either, and the parser folds the two together
 * when it reports. */
static const char *lex_err_msg;
static uint32_t lex_err_line;

const char *py_lex_err_msg(void) { return lex_err_msg; }
uint32_t py_lex_err_line(void) { return lex_err_line; }

static void set_error(const char *msg, uint32_t line)
{
    if (!lex_err_msg) {
        lex_err_msg = msg;
        lex_err_line = line;
    }
}

static py_token *new_token(uint8_t kind, uint8_t op, uint32_t line)
{
    /* The token list is released when the parse is done, but the tokens
     * themselves are scattered through the arena and the text they carry
     * has to stay for the tree - so they are permanent blocks, and the
     * list is given back as a whole (see py_lex_free). */
    py_token *t = (py_token *)py_alloc_perm((uint32_t)sizeof(py_token));
    if (!t)
        return 0;
    t->kind = kind;
    t->op = op;
    t->line = line;
    t->next = 0;
    if (tok_tail)
        tok_tail->next = t;
    else
        tok_head = t;
    tok_tail = t;
    return t;
}

/* Copy `n` bytes into a fresh arena block and NUL terminate it, so the
 * token owns its text. */
static char *dup_text(const char *s, uint32_t n)
{
    char *d = (char *)py_alloc(n + 1);
    if (!d)
        return 0;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* The keyword table.  A short list, so a linear scan is fine and the
 * table costs nothing. */
static const struct {
    const char *name;
    uint8_t id;
} keywords[] = {
    { "if", PY_K_IF }, { "elif", PY_K_ELIF }, { "else", PY_K_ELSE },
    { "while", PY_K_WHILE }, { "for", PY_K_FOR }, { "in", PY_K_IN },
    { "def", PY_K_DEF }, { "return", PY_K_RETURN },
    { "break", PY_K_BREAK }, { "continue", PY_K_CONTINUE }, { "pass", PY_K_PASS },
    { "del", PY_K_DEL },
    { "and", PY_K_AND }, { "or", PY_K_OR }, { "not", PY_K_NOT },
    { "True", PY_K_TRUE }, { "False", PY_K_FALSE }, { "None", PY_K_NONE_LIT },
};

static uint8_t keyword_id(const char *s, uint32_t n)
{
    for (unsigned i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
        if (strlen(keywords[i].name) == n &&
            strncmp(keywords[i].name, s, n) == 0)
            return keywords[i].id;
    }
    return PY_K_NONE;
}

static int is_name_start(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int is_name_char(int c)
{
    return is_name_start(c) || (c >= '0' && c <= '9');
}

static int is_digit(int c) { return c >= '0' && c <= '9'; }

/* Read one line's worth of tokens, handling the indentation.
 * Returns 0 on error. */
static int lex_line(const char **pp, const char *end)
{
    const char *p = *pp;

    /* Measure the indentation.  Only spaces count; a tab is an error,
     * because guessing where it puts the cursor is exactly the bug that
     * makes a program mean different things in different editors. */
    uint32_t col = 0;
    while (p < end && (*p == ' ' || *p == '\t')) {
        if (*p == '\t') {
            set_error("tabs are not allowed; use spaces for indentation", cur_line);
            return 0;
        }
        col++;
        p++;
    }

    /* A blank line or a comment-only line: no NEWLINE, no indentation
     * change.  This is what lets a comment block sit inside a function
     * without closing it.
     *
     * The comment text still has to be consumed.  Leaving it there would
     * make the caller read the same line again, forever: nothing about
     * the line would change, so no token would ever be produced. */
    if (p >= end || *p == '\n' || *p == '\r' || *p == '#') {
        if (p < end && *p == '#') {
            while (p < end && *p != '\n')
                p++;
        }
        if (p < end && *p == '\r')
            p++;
        if (p < end && *p == '\n') {
            p++;
            cur_line++;
        }
        *pp = p;
        return 1;
    }

    /* Compare the indentation with the stack. */
    if (col > indents[nindents]) {
        if (nindents + 1 >= (int)(sizeof(indents) / sizeof(indents[0]))) {
            set_error("too many levels of indentation", cur_line);
            return 0;
        }
        indents[++nindents] = col;
        if (!new_token(PY_T_INDENT, 0, cur_line))
            return 0;
    } else {
        while (col < indents[nindents]) {
            nindents--;
            if (!new_token(PY_T_DEDENT, 0, cur_line))
                return 0;
        }
        if (col != indents[nindents]) {
            set_error("unindent does not match any outer indentation level", cur_line);
            return 0;
        }
    }

    /* Now the tokens of the line.  A carriage return counts as whitespace:
     * a script written on Windows has CRLF line endings, and refusing to
     * read them would be refusing to read half the scripts in the world. */
    for (;;) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r')) {
            if (*p == '\t') {
                set_error("tabs are not allowed; use spaces for indentation", cur_line);
                return 0;
            }
            p++;
        }
        if (p >= end)
            break;
        if (*p == '#') {          /* comment to the end of the line */
            while (p < end && *p != '\n')
                p++;
            break;
        }
        if (*p == '\n')
            break;

        if (is_name_start(*p)) {
            const char *s = p;
            while (p < end && is_name_char(*p))
                p++;
            uint32_t n = (uint32_t)(p - s);
            uint8_t kw = keyword_id(s, n);
            if (kw != PY_K_NONE) {
                if (!new_token(PY_T_KEYWORD, kw, cur_line))
                    return 0;
            } else {
                py_token *t = new_token(PY_T_NAME, 0, cur_line);
                if (!t)
                    return 0;
                t->text = dup_text(s, n);
                if (!t->text)
                    return 0;
            }
            continue;
        }

        if (is_digit(*p)) {
            const char *s = p;
            int32_t v = 0;
            while (p < end && is_digit(*p)) {
                v = v * 10 + (*p - '0');
                p++;
            }
            py_token *t = new_token(PY_T_INT, 0, cur_line);
            if (!t)
                return 0;
            t->ival = v;
            (void)s;
            continue;
        }

        if (*p == '"' || *p == '\'') {
            char quote = *p++;
            const char *s = p;
            while (p < end && *p != quote) {
                if (*p == '\n') {
                    set_error("unterminated string literal", cur_line);
                    return 0;
                }
                p++;
            }
            if (p >= end) {
                set_error("unterminated string literal", cur_line);
                return 0;
            }
            uint32_t n = (uint32_t)(p - s);
            py_token *t = new_token(PY_T_STR, 0, cur_line);
            if (!t)
                return 0;
            t->text = dup_text(s, n);
            if (!t->text)
                return 0;
            p++;                /* the closing quote */
            continue;
        }

        /* Operators. */
        uint8_t op = 0;
        int two = 0;
        switch (*p) {
        case '+': op = (p + 1 < end && p[1] == '=') ? PY_O_PLUSEQ : PY_O_PLUS;
                  two = (p + 1 < end && p[1] == '='); break;
        case '-': op = (p + 1 < end && p[1] == '=') ? PY_O_MINUSEQ : PY_O_MINUS;
                  two = (p + 1 < end && p[1] == '='); break;
        case '*': op = (p + 1 < end && p[1] == '=') ? PY_O_STAREQ : PY_O_STAR;
                  two = (p + 1 < end && p[1] == '='); break;
        case '/': op = (p + 1 < end && p[1] == '=') ? PY_O_SLASHEQ : PY_O_SLASH;
                  two = (p + 1 < end && p[1] == '='); break;
        case '%': op = (p + 1 < end && p[1] == '=') ? PY_O_PERCENTEQ : PY_O_PERCENT;
                  two = (p + 1 < end && p[1] == '='); break;
        case '=': op = (p + 1 < end && p[1] == '=') ? PY_O_EQ : PY_O_ASSIGN;
                  two = (p + 1 < end && p[1] == '='); break;
        case '!':
            if (p + 1 < end && p[1] == '=') {
                op = PY_O_NE;
                two = 1;
            } else {
                set_error("unexpected '!'", cur_line);
                return 0;
            }
            break;
        case '<': op = (p + 1 < end && p[1] == '=') ? PY_O_LE : PY_O_LT;
                  two = (p + 1 < end && p[1] == '='); break;
        case '>': op = (p + 1 < end && p[1] == '=') ? PY_O_GE : PY_O_GT;
                  two = (p + 1 < end && p[1] == '='); break;
        case '(': op = PY_O_LPAREN; paren_depth++; break;
        case ')': op = PY_O_RPAREN; paren_depth--; break;
        case '[': op = PY_O_LBRACKET; paren_depth++; break;
        case ']': op = PY_O_RBRACKET; paren_depth--; break;
        case '{': op = PY_O_LBRACE; paren_depth++; break;
        case '}': op = PY_O_RBRACE; paren_depth--; break;
        case ',': op = PY_O_COMMA; break;
        case ':': op = PY_O_COLON; break;
        case '.': op = PY_O_DOT; break;
        default:
            set_error("unexpected character", cur_line);
            return 0;
        }
        if (!new_token(PY_T_OP, op, cur_line))
            return 0;
        p += two ? 2 : 1;
    }

    *pp = p;

    /* End of the line.  Inside brackets a newline is not a statement end,
     * so keep reading until the brackets balance. */
    if (paren_depth > 0) {
        while (p < end && *p != '\n')
            p++;
        if (p < end) {
            p++;
            cur_line++;
        }
        *pp = p;
        return 1;
    }

    if (!new_token(PY_T_NEWLINE, 0, cur_line))
        return 0;
    if (p < end && *p == '\n') {
        p++;
        cur_line++;
    }
    *pp = p;
    return 1;
}

py_token *py_lex_all(const char *src, uint32_t len)
{
    const char *p = src;
    const char *end = src + len;

    cur_line = 1;
    lex_err_msg = 0;
    lex_err_line = 0;
    tok_head = tok_tail = 0;
    nindents = 0;
    paren_depth = 0;
    indents[0] = 0;

    while (p < end) {
        if (!lex_line(&p, end))
            return 0;
    }

    /* Close every open block, then end the file. */
    while (nindents > 0) {
        nindents--;
        if (!new_token(PY_T_DEDENT, 0, cur_line))
            return 0;
    }
    if (!new_token(PY_T_NEWLINE, 0, cur_line))
        return 0;
    if (!new_token(PY_T_EOF, 0, cur_line))
        return 0;
    return tok_head;
}
