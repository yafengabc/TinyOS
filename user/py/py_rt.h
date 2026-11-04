/* py_rt.h - the runtime the bytecode runs on.
 *
 * The bytecode does not know how a list works, how a name is found, or
 * what `print` does.  It knows the shape of those things - a scope has
 * names, a builtin takes arguments, a method takes an object and its
 * arguments - and this header is where those shapes are written down.
 *
 * The scopes are the same ones the tree-walking evaluator used, and
 * they are deliberately dynamic: a name is assigned where it is already
 * bound, searching outward, and defined in the current scope otherwise.
 * A compiler that picked a slot at compile time would change what the
 * language means, so the lookup happens at run time, here.
 */

#ifndef TINYOS_PY_RT_H
#define TINYOS_PY_RT_H

#include "types.h"
#include "py_obj.h"

/* A scope.  The names are the program's name table entries, so a name is
 * compared by pointer rather than by content. */

/* A scope with a parent.  The parent is the scope a function was
 * defined in, so a function body sees the names around it. */
py_scope *py_scope_new(py_scope *parent);

/* Define a name in this scope, replacing any binding it has here.
 * The scope takes over the reference to the value. */
void py_scope_define(py_scope *s, const char *name, py_value v);

/* Assign a name: where it is already bound, searching outward, assign
 * it there; otherwise define it in this scope.  The scope takes over
 * the reference to the value. */
void py_scope_assign(py_scope *s, const char *name, py_value v);

/* Look a name up, searching outward.  Returns 0 when it is not bound;
 * otherwise the value is borrowed and the caller must incref it. */
int py_scope_lookup(py_scope *s, const char *name, py_value *out);

/* Remove a name from this scope.  Returns 0 when it was not bound
 * here. */
int py_scope_delete(py_scope *s, const char *name);

/* Whether a name is bound in this scope. */
int py_scope_bound(py_scope *s, const char *name);

/* Release a scope and the values it holds. */
void py_scope_release(py_scope *s);

/* A call in progress.  The argument array holds the positional
 * arguments; the keyword arguments are kept in parallel arrays so a
 * builtin can look one up by name without building a dictionary per
 * call. */
struct py_call {
    py_value *args;
    int argc;
    py_value *kwvals;
    char **kwnames;
    int nkwargs;
};

/* A builtin: a name and the function that implements it. */
typedef py_value (*builtin_fn)(struct py_call *c);

typedef struct {
    const char *name;
    builtin_fn fn;
} py_builtin;

/* The builtins.  They are ordinary names in the module scope, exactly
 * as they were when the evaluator walked the tree, so a script can
 * shadow them. */
extern const py_builtin py_builtins[];
uint32_t py_builtins_count(void);

/* Put the builtins into a scope.  The names have to be the ones the
 * program's name table holds, or a script's own `print` would not be
 * the same name as the builtin's. */
void py_scope_define_builtins(py_scope *s, const char **names);

/* The operators.  Each takes its operands and returns the result; the
 * caller owns the result. */
py_value py_op_add(py_value a, py_value b, uint32_t line);
py_value py_op_sub(py_value a, py_value b, uint32_t line);
py_value py_op_mul(py_value a, py_value b, uint32_t line);
py_value py_op_div(py_value a, py_value b, uint32_t line);
py_value py_op_mod(py_value a, py_value b, uint32_t line);
py_value py_op_neg(py_value a, uint32_t line);
int      py_op_eq(py_value a, py_value b);
int      py_op_ne(py_value a, py_value b);
int      py_op_lt(py_value a, py_value b, uint32_t line);
int      py_op_le(py_value a, py_value b, uint32_t line);
int      py_op_gt(py_value a, py_value b, uint32_t line);
int      py_op_ge(py_value a, py_value b, uint32_t line);
int      py_op_in(py_value item, py_value container, uint32_t line);

/* The items an iterable yields, as a fresh list.  Copying the list
 * first means a loop body that appends to the list it is iterating does
 * not change the loop underfoot. */
py_value py_iterable_items(py_value v, uint32_t line);

/* A method call: the method is looked up by name in the table for the
 * object's type.  Returns 0 when the object has no such method, and
 * leaves the message in the error slot. */
int py_call_method(py_value obj, const char *name, py_value *args, int argc,
                   uint32_t line, py_value *out);

/* The error slot.  The first error wins: a failed operation leaves the
 * rest of the program unevaluated, and the one that failed is the one
 * worth reporting. */
void py_rt_error(uint32_t line, const char *msg);

/* Build an error message out of up to five pieces, so that a name or a
 * type can be woven into it. */
void rt_error5(uint32_t line, const char *a, const char *b, const char *c,
               const char *d, const char *e);
const char *py_rt_error_msg(void);
uint32_t py_rt_error_line(void);
void py_rt_error_clear(void);

#endif
