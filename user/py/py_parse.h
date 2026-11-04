/* py_parse.h - tokens and the parser.
 *
 * The lexer turns source text into a token list, the parser turns that
 * into a tree.  Python's block structure is indentation, so the lexer has
 * to emit INDENT and DEDENT tokens the way CPython's does: the indentation
 * of a line is compared against a stack, and every extra level pushes an
 * INDENT while every level popped emits a DEDENT.  Blank lines and lines
 * that hold only a comment are skipped entirely - they produce no NEWLINE
 * and no indentation change, the same as CPython.
 */

#ifndef TINYOS_PY_PARSE_H
#define TINYOS_PY_PARSE_H

#include "types.h"
#include "py_ast.h"

typedef enum {
    PY_T_EOF = 0,
    PY_T_NEWLINE,
    PY_T_INDENT,
    PY_T_DEDENT,
    PY_T_NAME,
    PY_T_INT,
    PY_T_STR,
    PY_T_OP,
    PY_T_KEYWORD
} py_tok_kind;

/* Keywords. */
enum {
    PY_K_NONE = 0,
    PY_K_IF, PY_K_ELIF, PY_K_ELSE,
    PY_K_WHILE, PY_K_FOR, PY_K_IN,
    PY_K_DEF, PY_K_RETURN, PY_K_BREAK, PY_K_CONTINUE, PY_K_PASS, PY_K_DEL,
    PY_K_AND, PY_K_OR, PY_K_NOT,
    PY_K_TRUE, PY_K_FALSE, PY_K_NONE_LIT
};

/* Operators. */
enum {
    PY_O_NONE = 0,
    PY_O_PLUS, PY_O_MINUS, PY_O_STAR, PY_O_SLASH, PY_O_PERCENT,
    PY_O_ASSIGN,
    PY_O_EQ, PY_O_NE, PY_O_LT, PY_O_LE, PY_O_GT, PY_O_GE,
    PY_O_LPAREN, PY_O_RPAREN, PY_O_LBRACKET, PY_O_RBRACKET,
    PY_O_LBRACE, PY_O_RBRACE,
    PY_O_COMMA, PY_O_COLON, PY_O_DOT,
    PY_O_PLUSEQ, PY_O_MINUSEQ, PY_O_STAREQ, PY_O_SLASHEQ, PY_O_PERCENTEQ
};

/* A token is 16 bytes, which is one size class exactly: the token list of
 * a script is one of the two things that fill the interpreter's arena, so
 * a rounding loss here is a rounding loss everywhere.
 *
 * There is no length field: the text is NUL terminated, and the lexer does
 * not support escape sequences, so the length of a string literal is
 * strlen() of its text.  The line number is 16 bits - no script is that
 * long. */
typedef struct py_token {
    uint8_t kind;
    uint8_t op;          /* operator or keyword id */
    uint16_t line;
    int32_t ival;
    char *text;          /* name text, or string literal bytes (NUL ended) */
    struct py_token *next;
} py_token;

/* Parse a whole program.  Returns the module scope, or 0 on a syntax
 * error.  The error message and line are left in py_err_msg()/py_err_line()
 * so the caller can report them the way Python does. */
py_scope *py_parse(const char *src, uint32_t len);

const char *py_err_msg(void);
uint32_t py_err_line(void);

/* The lexer, exposed to the parser.  It keeps its own error state, because
 * a syntax error can come from the lexer (a bad indent) or from the parser
 * (a bad expression); py_err_msg() folds the two together. */
py_token *py_lex_all(const char *src, uint32_t len);
const char *py_lex_err_msg(void);
uint32_t py_lex_err_line(void);

#endif
