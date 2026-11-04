/* py_eval.h - running a parsed program.
 *
 * The evaluator walks the tree the parser built.  It has three jobs:
 * evaluate expressions, execute statements, and call functions - user
 * defined ones and the builtins.
 *
 * Scopes are lexical: a function body sees the names of the scope it was
 * defined in, and a name is looked up outward from where it is used.
 */

#ifndef TINYOS_PY_EVAL_H
#define TINYOS_PY_EVAL_H

#include "types.h"
#include "py_ast.h"
#include "py_obj.h"

/* A call in progress.  The argument array holds the positional arguments;
 * the keyword arguments are kept in parallel arrays so a builtin can look
 * one up by name without building a dictionary per call. */
struct py_call {
    py_value *args;
    int argc;
    py_value *kwvals;
    char **kwnames;
    int nkwargs;
};

/* Run a module scope.  Returns 0 on success.  On a runtime error it
 * returns -1, with the message in py_eval_error() and the line it
 * happened on in py_eval_line(). */
int py_run(py_scope *mod);

const char *py_eval_error(void);
uint32_t py_eval_line(void);

#endif
