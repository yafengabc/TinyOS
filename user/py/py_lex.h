/* py_lex.h - the tokens the compiler reads.
 *
 * The lexer turns source text into a flat array of tokens.  The array is
 * one allocation that grows by doubling, and the text of a token points
 * into the source buffer rather than being copied: a script of a hundred
 * lines has several hundred tokens, and copying each name into its own
 * arena block cost more than the tokens themselves.
 *
 * The text is not NUL terminated - it is the run of characters between
 * the delimiters - so every token carries its length.  (The lexer does
 * not support escape sequences, so the length is the only thing a string
 * literal needs.)
 *
 * Python's blocks are indentation, so the lexer has to emit INDENT and
 * DEDENT the way CPython's tokenizer does: the indentation of a line is
 * compared against a stack, and every extra level pushes an INDENT while
 * every level popped emits a DEDENT.  Blank lines and lines that hold
 * only a comment are skipped entirely - they produce no NEWLINE and no
 * indentation change, the same as CPython.
 */

#ifndef TINYOS_PY_LEX_H
#define TINYOS_PY_LEX_H

#include "types.h"

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

/* A token is 16 bytes: kind, op, the line it is on, the length of its
 * text, the value of an integer literal, and the text itself. */
typedef struct py_token {
    uint8_t kind;
    uint8_t op;          /* operator or keyword id */
    uint16_t line;
    uint16_t len;        /* length of the text */
    int32_t ival;        /* the value of an integer literal */
    const char *text;    /* into the source buffer, not NUL terminated */
} py_token;

/* The token array.  `toks` is 0 when the source could not be lexed. */
typedef struct py_tokens {
    py_token *toks;
    uint32_t n;
    uint32_t cap;
} py_tokens;

/* Lex the whole source.  Returns 0 on a lexical error; the message and
 * the line are in py_lex_err_msg() and py_lex_err_line(). */
int py_lex_all(const char *src, uint32_t len, py_tokens *out);

const char *py_lex_err_msg(void);
uint32_t py_lex_err_line(void);

#endif
