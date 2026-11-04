/* py_code.h - the bytecode: what the compiler emits and the VM runs.
 *
 * The interpreter used to walk the tree the parser built.  Now the parser
 * emits bytecode as it goes and the tree never exists at all: a hundred-line
 * script used to cost about 45 KiB of tree and now costs about 3 KiB of
 * code.  That is the whole reason for the change - the tree was the largest
 * thing in the interpreter's memory, and on a microcontroller the largest
 * thing in memory is the thing that decides how long a script can be.
 *
 * The bytecode is a stack machine.  Every instruction is four bytes:
 *
 *     +--------+--------+----------------+----------------+
 *     | op     | a      | b (low)        | b (high)       |
 *     +--------+--------+----------------+----------------+
 *
 * `op` is the opcode, `a` is a small operand (an argument count) and `b`
 * is a sixteen bit operand (a constant index, a name index, a jump
 * distance).  Fixed width costs a byte or two per instruction against a
 * variable length encoding, and buys a much simpler interpreter loop: the
 * program counter is an instruction index, so a jump target is a plain
 * number and a line number is found by index too.
 *
 * A jump is a *relative* distance in instructions from the instruction
 * after the jump.  Sixteen bits of signed distance is 32 KiB of code in
 * either direction, which is more than any script this interpreter will
 * ever run; a program that would need more is refused at compile time
 * rather than silently miscompiled.
 *
 * The names in the bytecode are indexes into the program's name table,
 * and the constants are indexes into its constant pool.  Neither is
 * resolved to a slot at compile time, because this subset's scoping is
 * decided at run time: a name is assigned where it is already bound,
 * searching outward, and defined in the current scope otherwise.  A
 * compiler that picked a slot at compile time would change what the
 * language means, so the VM does the lookup the same way the old
 * evaluator did.
 *
 * The compiler lives in py_compile.c and the VM in py_vm.c.  The values
 * the two of them share - the tagged union, the objects, the scopes - are
 * in py_obj.h and py_rt.h.
 */

#ifndef TINYOS_PY_CODE_H
#define TINYOS_PY_CODE_H

#include "types.h"
#include "py_lex.h"
#include "py_obj.h"

/* The opcodes.
 *
 * The ones with a name in the comment take their operand from `b`; the
 * ones that take an argument count take it from `a`. */
enum {
    OP_PUSH_CONST = 1,   /* b = constant index */
    OP_PUSH_NONE,        /* - */
    OP_PUSH_INT0,        /* - : push the number zero */
    OP_PUSH_TRUE,        /* - */
    OP_PUSH_FALSE,       /* - */
    OP_LOAD,             /* b = name index: push the value of the name */
    OP_STORE,            /* b = name index: pop a value, assign the name */
    OP_DEFINE,           /* b = name index: pop a value, define it here */
    OP_DELETE,           /* b = name index: remove the name */
    OP_ADD,              /* - : pop b, pop a, push a + b */
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_NEG,              /* - : pop a, push -a */
    OP_NOT,              /* - : pop a, push not a */
    OP_EQ,               /* - : pop b, pop a, push a == b */
    OP_NE,
    OP_LT,
    OP_LE,
    OP_GT,
    OP_GE,
    OP_IN,
    OP_JUMP,             /* b = relative distance in instructions */
    OP_JUMP_IF_FALSE,    /* b: pop a value, jump if it is false */
    OP_JUMP_IF_TRUE,     /* b: pop a value, jump if it is true */
    OP_CALL,             /* a = argument count */
    OP_CALL_KW,          /* a = argument count, b = call plan offset */
    OP_CALL_METHOD,      /* a = argument count, b = method name index */
    OP_GET_ATTR,         /* b = name index: an attribute that is not called */
    OP_RETURN,           /* - : pop the return value */
    OP_MAKE_LIST,        /* b = number of elements */
    OP_MAKE_DICT,        /* b = number of key/value pairs */
    OP_GET_ITEM,         /* - : pop the index, pop the container, push the item */
    OP_SET_ITEM,         /* - : pop the index, the container and the value */
    OP_DEL_ITEM,         /* - : pop the index and the container */
    OP_MAKE_FUNC,        /* b = code index: push a function object */
    OP_FOR_PREP,         /* - : pop an iterable, start a for loop */
    OP_FOR_NEXT,         /* b = distance to the end of the loop */
    OP_POP_ITERS,        /* a = number of loops to close */
    OP_DUP,              /* - : duplicate the top of the stack */
    OP_POP               /* - : drop the top of the stack */
};

#define PY_OPCODE_MAX OP_POP

/* An instruction.  Four bytes: see the comment at the top. */
typedef struct py_insn {
    uint8_t op;
    uint8_t a;
    uint16_t b;
} py_insn;

/* One function's code.
 *
 * Every function - and the module itself - gets one of these.  The
 * parameters are indexes into the program's name table, so they cost
 * nothing beyond the two bytes each.
 *
 * The line table says which source line each instruction came from, so
 * that an error can name the line it happened on.  One entry per
 * instruction: a run-length table would be smaller, but the line is
 * wanted for every instruction that fails, and a flat array is a single
 * lookup with no search. */
typedef struct py_code {
    uint8_t *ins;             /* the instructions */
    uint32_t nins;
    uint32_t cins;
    uint16_t *lines;          /* the source line of each instruction */

    uint16_t *params;         /* the parameter names, in order */
    uint32_t nparams;

    const char *name;         /* the function's name, for error messages */
} py_code;

/* A function object: the code it runs, and the scope it was defined
 * in.  The scope is what makes a closure work - a function that refers
 * to a name of the scope around it has to see that scope. */
struct py_funcdef {
    uint32_t code;         /* the index into the program's code objects */
    py_scope *closure;     /* the scope it was defined in */
};

/* A whole program: the module's code, the code of every function, and
 * the tables the instructions index into. */
typedef struct py_prog {
    py_tokens *toks;          /* the token stream it was compiled from */
    py_code **codes;          /* the code objects; [0] is the module */
    uint32_t ncodes;
    uint32_t ccodes;

    py_value *consts;         /* the constant pool */
    uint32_t nconsts;
    uint32_t cconsts;

    const char **names;       /* the name table */
    uint32_t nnames;
    uint32_t cnames;

    uint16_t *plans;          /* the call plans: see py_compile.c */
    uint32_t nplans;
    uint32_t cplans;

    uint16_t builtin_names[32];   /* the builtins, in the name table */
    uint32_t nbuiltin_names;
} py_prog;

/* Compile a token stream into bytecode.  Returns 0 on failure, with the
 * message and the line in py_compile_error() and py_compile_error_line(). */
py_prog *py_compile(py_tokens *toks);

const char *py_compile_error(void);
uint32_t py_compile_error_line(void);

/* Run a compiled program.  Returns 0 on success.  On a runtime error it
 * returns -1, with the message and the line in py_vm_error() and
 * py_vm_error_line(). */
int py_vm_run(py_prog *prog);

const char *py_vm_error(void);
uint32_t py_vm_error_line(void);

/* How much the compiled program costs, so the interpreter can say so
 * instead of leaving the reader to guess. */
uint32_t py_prog_insns(py_prog *prog);
uint32_t py_prog_bytes(py_prog *prog);

#endif
