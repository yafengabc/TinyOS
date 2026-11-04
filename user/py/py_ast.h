/* py_ast.h - the interpreter's syntax tree.
 *
 * Nodes are allocated from the interpreter's arena and never freed: the
 * tree lives for as long as the program runs, and the process exits when
 * the script ends.  One node type with a union keeps the tree small, which
 * matters when the whole arena is a few tens of kilobytes: on a 192 KiB
 * microcontroller a script and its tree have to fit in what is left of
 * the task window.
 *
 * The node is 48 bytes.  What did not fit in that is in the union: the
 * fields only one kind of node uses.  A string literal needs a value, a
 * call needs an argument list, a dict literal needs two chains, a def
 * needs its parameters and its scope.
 */

#ifndef TINYOS_PY_AST_H
#define TINYOS_PY_AST_H

#include "types.h"
#include "py_obj.h"

typedef enum {
    /* expressions */
    PY_E_INT,
    PY_E_STR,
    PY_E_NAME,
    PY_E_BOOL,
    PY_E_NONE,
    PY_E_LIST,      /* a = elements, chained by ->next */
    PY_E_GROUP,     /* a parenthesised group: '(a)' is a, '[a]' is a list */
    PY_E_DICT,      /* u.dict.keys and u.dict.vals, two parallel chains */
    PY_E_BINOP,     /* a op b */
    PY_E_UNARY,     /* op a */
    PY_E_COMPARE,   /* a op b */
    PY_E_BOOLOP,    /* a op b, op is PY_OP_AND / PY_OP_OR */
    PY_E_CALL,      /* a = function, u.call.args = arguments */
    PY_E_INDEX,     /* a = container, b = index */
    PY_E_ATTR,      /* a = object, name = attribute */

    /* statements */
    PY_S_EXPR,      /* a = expression */
    PY_S_ASSIGN,    /* a = target, b = value */
    PY_S_AUGASSIGN, /* a = target, op, b = value */
    PY_S_IF,        /* a = condition, b = body, c = else (may be 0) */
    PY_S_WHILE,     /* a = condition, b = body */
    PY_S_FOR,       /* name = variable, a = iterable, b = body */
    PY_S_FUNC,      /* name, u.fn.nparams, u.fn.body = scope */
    PY_S_RETURN,    /* a = value (may be 0) */
    PY_S_BREAK,
    PY_S_CONTINUE,
    PY_S_PASS,
    PY_S_DEL,       /* a = the target */
    PY_S_BLOCK      /* list = statement list */
} py_node_kind;

/* Binary and comparison operators. */
enum {
    PY_OP_NONE = 0,
    PY_OP_ADD, PY_OP_SUB, PY_OP_MUL, PY_OP_DIV, PY_OP_MOD,
    PY_OP_EQ, PY_OP_NE, PY_OP_LT, PY_OP_LE, PY_OP_GT, PY_OP_GE,
    PY_OP_AND, PY_OP_OR,
    PY_OP_IN
};

typedef struct py_node py_node;
typedef struct py_scope py_scope;

struct py_node {
    uint8_t kind;
    uint8_t op;
    uint32_t line;
    int32_t ival;         /* int literal, or a flag on the node */
    char *name;           /* name, attribute, or function name */
    py_node *a;
    py_node *b;
    py_node *c;
    py_node *list;        /* first statement of a block */
    py_node *next;        /* next sibling in a statement list */
    union {
        py_value sval;                                  /* PY_E_STR */
        struct { py_node *args; } call;                  /* PY_E_CALL */
        struct { py_node *keys; py_node *vals; } dict;    /* PY_E_DICT */
        struct {                                        /* PY_S_FUNC */
            char **params;
            uint32_t nparams;
            py_scope *body;
        } fn;
    } u;
};

/* A scope: the names a statement block can see.  The module scope holds
 * the globals; a function body gets a scope of its own whose parent is
 * the module scope. */
struct py_scope {
    py_node *body;        /* the statements */
    py_scope *parent;
};

#endif
