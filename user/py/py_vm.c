/* py_vm.c - the virtual machine that runs the bytecode.
 *
 * The machine is a stack machine.  Every instruction is four bytes and
 * does one thing; the loop is a switch over the opcode.
 *
 * Three pieces of state:
 *
 *   - the operand stack, where the values of expressions live while
 *     they are being computed;
 *   - the scopes, which are the same dynamic scopes the tree-walking
 *     evaluator used;
 *   - the call stack, which is the C stack: a call runs the callee's
 *     code in a C function, so the depth of the recursion is the depth
 *     of the Python recursion, exactly as it was before.
 *
 * The scopes are why the bytecode carries names and not slot numbers.  A
 * name is assigned where it is already bound, searching outward, and
 * defined in the current scope otherwise - and whether a name is bound
 * somewhere outward is not known until the program runs.  So the lookup
 * happens here, at run time, exactly as it did before.
 */

#include "py_code.h"
#include "py_rt.h"
#include "py_mem.h"
#include "py_str.h"
#include "lib.h"

/* ---- the machine --------------------------------------------------------- */

/* The operand stack, where the values of expressions live while they
 * are being computed.  It grows as needed and is shared by every call:
 * each call remembers where its own part starts. */
static py_value *vstack;
static uint32_t vsp;
static uint32_t cvstack;

/* The error the machine stopped on. */
static const char *vm_err;
static uint32_t vm_err_line;

const char *py_vm_error(void) { return vm_err; }
uint32_t py_vm_error_line(void) { return vm_err_line; }

/* Stop with an error.  The first error wins: a failed operation leaves
 * the rest of the program unevaluated, and the one that failed is the
 * one worth reporting. */
static void vm_fail(uint32_t line, const char *msg)
{
    if (vm_err)
        return;
    py_rt_error(line, msg);
    vm_err = py_rt_error_msg();
    vm_err_line = py_rt_error_line();
}

/* The operand stack. */
static int vm_push(py_value v)
{
    if (vsp == cvstack) {
        uint32_t ncap = cvstack ? cvstack * 2 : 64;
        py_value *ns = (py_value *)py_alloc(ncap * (uint32_t)sizeof(py_value));
        if (!ns) {
            vm_fail(0, "MemoryError: out of memory");
            return 0;
        }
        if (vsp)
            memcpy(ns, vstack, vsp * sizeof(py_value));
        if (vstack)
            py_free_auto(vstack);
        vstack = ns;
        cvstack = ncap;
    }
    vstack[vsp++] = v;
    return 1;
}

static py_value vm_pop(void)
{
    if (!vsp) {
        vm_fail(0, "InternalError: the stack is empty");
        return py_none();
    }
    return vstack[--vsp];
}

/* The line an instruction came from, for the error message. */
static uint32_t vm_line(const py_code *code, uint32_t pc)
{
    if (!code->lines || pc >= code->nins)
        return 0;
    return code->lines[pc];
}

/* ---- running code -------------------------------------------------------- */

/* Run a block of code in a scope.  Returns 0 on success, -1 on an
 * error.  A `return` in the code ends the block and leaves its value
 * in `out`. */
static int vm_run(py_prog *prog, const py_code *code, py_scope *scope,
                  py_value *out);

/* Bind the arguments of a call to the parameters of a function.
 *
 * The positional arguments bind left to right; a keyword argument binds
 * by name.  A function that is missing an argument, or that is given
 * one it does not have, is an error - the same errors the old
 * evaluator reported. */
static int bind_params(py_prog *prog, const py_code *fc, py_scope *scope,
                       py_value *args, int argc, char **kwnames, int nkwargs,
                       uint32_t line)
{
    uint32_t nparams = fc->nparams;

    if ((uint32_t)argc > nparams) {
        py_rt_error(line, "TypeError: too many arguments");
        return 0;
    }

    for (uint32_t i = 0; i < (uint32_t)argc; i++) {
        /* The scope takes a reference of its own: the caller's copy is
         * released by the caller, and the scope's copy by the scope. */
        py_incref(args[i]);
        py_scope_define(scope, prog->names[fc->params[i]], args[i]);
    }

    for (int k = 0; k < nkwargs; k++) {
        const char *name = kwnames[k];
        uint32_t found = nparams;
        for (uint32_t i = 0; i < nparams; i++) {
            if (prog->names[fc->params[i]] == name) {
                found = i;
                break;
            }
        }
        if (found == nparams) {
            py_rt_error(line, "TypeError: unexpected keyword argument");
            return 0;
        }
        if (py_scope_bound(scope, name)) {
            py_rt_error(line, "TypeError: multiple values for argument");
            return 0;
        }
        py_incref(args[argc + k]);
        py_scope_define(scope, name, args[argc + k]);
    }

    /* Every parameter has to be bound. */
    for (uint32_t i = 0; i < nparams; i++) {
        if (!py_scope_bound(scope, prog->names[fc->params[i]])) {
            py_rt_error(line, "TypeError: missing argument");
            return 0;
        }
    }
    return 1;
}

/* Make a function object: the code it runs, and the scope it was
 * defined in. */
static py_value make_function(py_prog *prog, uint32_t code_index,
                              py_scope *closure, const char *name)
{
    py_obj *o = (py_obj *)py_alloc((uint32_t)sizeof(py_obj));
    py_value v;
    v.type = PY_FUNC;
    if (!o) {
        v.u.obj = 0;
        return v;
    }
    o->refs = 1;
    o->type = PY_FUNC;
    o->u.func.name = name;
    o->u.func.code = code_index;
    o->u.func.closure = closure;
    v.u.obj = o;
    return v;
}

/* The calls, which the instruction loop needs before they are written. */
static int vm_call(py_prog *prog, py_scope *scope, int argc, int nkw,
                   uint32_t plan, uint32_t line, py_value *out);
static int vm_call_method(py_prog *prog, py_scope *scope, int argc,
                          uint32_t nameidx, uint32_t line, py_value *out);

/* ---- indexing and the containers ------------------------------------------ */

/* The value at an index of a container.  The three kinds of container
 * behave the way Python's do: a list and a string take an integer index
 * and count from the end when it is negative, and a dict takes any key
 * and reports a missing one as a KeyError. */
static int vm_index(py_value cont, py_value idx, uint32_t line, py_value *out)
{
    if (cont.type == PY_LIST) {
        int32_t i;
        if (idx.type == PY_BOOL)
            i = idx.u.i ? 1 : 0;
        else if (idx.type == PY_INT)
            i = idx.u.i;
        else {
            rt_error5(line, "TypeError: list indices must be integers",
                      0, 0, 0, 0);
            vm_fail(line, "");
            return 0;
        }
        int32_t len = py_list_len(cont);
        if (i < 0)
            i += len;
        if (i < 0 || i >= len) {
            rt_error5(line, "IndexError: list index out of range", 0, 0, 0, 0);
            vm_fail(line, "");
            return 0;
        }
        py_value v = py_list_get(cont, i);
        py_incref(v);
        *out = v;
        return 1;
    }

    if (cont.type == PY_STR) {
        int32_t i;
        if (idx.type == PY_BOOL)
            i = idx.u.i ? 1 : 0;
        else if (idx.type == PY_INT)
            i = idx.u.i;
        else {
            rt_error5(line, "TypeError: string indices must be integers",
                      0, 0, 0, 0);
            vm_fail(line, "");
            return 0;
        }
        py_str *s = &cont.u.obj->u.str;
        if (i < 0)
            i += (int32_t)s->len;
        if (i < 0 || i >= (int32_t)s->len) {
            rt_error5(line, "IndexError: string index out of range", 0, 0, 0, 0);
            vm_fail(line, "");
            return 0;
        }
        *out = py_new_strn(s->bytes + i, 1);
        return 1;
    }

    if (cont.type == PY_DICT) {
        py_value v;
        if (py_dict_get(cont, idx, &v)) {
            py_incref(v);
            *out = v;
            return 1;
        }
        /* A missing key is an error, the way Python reports it. */
        py_value r = py_repr(idx);
        const char *txt = "?";
        if (r.type == PY_STR)
            txt = r.u.obj->u.str.bytes;
        rt_error5(line, "KeyError: ", txt, 0, 0, 0);
        py_decref(r);
        vm_fail(line, "");
        return 0;
    }

    rt_error5(line, "TypeError: '", py_type_name(cont),
              "' object is not subscriptable", 0, 0);
    vm_fail(line, "");
    return 0;
}

/* Store a value at an index of a container. */
static int vm_set_item(py_value cont, py_value idx, py_value val, uint32_t line)
{
    if (cont.type == PY_LIST) {
        int32_t i;
        if (idx.type == PY_BOOL)
            i = idx.u.i ? 1 : 0;
        else if (idx.type == PY_INT)
            i = idx.u.i;
        else {
            rt_error5(line, "TypeError: list indices must be integers",
                      0, 0, 0, 0);
            vm_fail(line, "");
            return 0;
        }
        int32_t len = py_list_len(cont);
        if (i < 0)
            i += len;
        if (i < 0 || i >= len) {
            rt_error5(line, "IndexError: list assignment index out of range",
                      0, 0, 0, 0);
            vm_fail(line, "");
            return 0;
        }
        py_list_set(cont, i, val);
        return 1;
    }

    if (cont.type == PY_DICT) {
        py_dict_set(cont, idx, val);
        return 1;
    }

    rt_error5(line, "TypeError: '", py_type_name(cont),
              "' object does not support item assignment", 0, 0);
    vm_fail(line, "");
    return 0;
}

/* Remove the item at an index of a container. */
static int vm_del_item(py_value cont, py_value idx, uint32_t line)
{
    if (cont.type == PY_DICT) {
        if (!py_dict_del(cont, idx)) {
            rt_error5(line, "KeyError: key not found", 0, 0, 0, 0);
            vm_fail(line, "");
            return 0;
        }
        return 1;
    }

    if (cont.type == PY_LIST) {
        int32_t i;
        if (idx.type == PY_BOOL)
            i = idx.u.i ? 1 : 0;
        else if (idx.type == PY_INT)
            i = idx.u.i;
        else {
            rt_error5(line, "TypeError: list indices must be integers",
                      0, 0, 0, 0);
            vm_fail(line, "");
            return 0;
        }
        int32_t len = py_list_len(cont);
        int32_t j = (i < 0) ? i + len : i;
        if (j < 0 || j >= len) {
            rt_error5(line, "IndexError: list assignment index out of range",
                      0, 0, 0, 0);
            vm_fail(line, "");
            return 0;
        }
        /* The list hands the element over; releasing it is ours. */
        py_value v = py_list_pop(cont, j);
        py_decref(v);
        return 1;
    }

    rt_error5(line, "TypeError: '", py_type_name(cont),
              "' object does not support item deletion", 0, 0);
    vm_fail(line, "");
    return 0;
}

/* The instructions.
 *
 * The operand stack holds the values of expressions while they are
 * being computed.  A call leaves the callee and the arguments on it
 * and the result on top:
 *
 *     <callee>            ; pushed by the expression that names it
 *     <arg0> ... <argN>   ; pushed by the arguments, in order
 *     CALL n              ; pops them, pushes the result
 *
 * A method call is the same, with the object where the callee would
 * be:
 *
 *     <object>
 *     <arg0> ... <argN>
 *     CALL_METHOD n       ; the method name is in the instruction
 */
static int vm_run(py_prog *prog, const py_code *code, py_scope *scope,
                  py_value *out)
{
    uint32_t pc = 0;

    while (pc < code->nins) {
        uint32_t line = vm_line(code, pc);
        const uint8_t *raw = &code->ins[pc * 4];
        uint8_t op = raw[0];
        uint8_t a = raw[1];
        uint16_t b = (uint16_t)(raw[2] | (raw[3] << 8));
        pc++;

        switch (op) {
        case OP_PUSH_CONST: {
            py_value v = prog->consts[b];
            py_incref(v);
            if (!vm_push(v))
                goto fail;
            break;
        }

        case OP_PUSH_NONE:
            if (!vm_push(py_none()))
                goto fail;
            break;

        case OP_PUSH_INT0:
            /* The zero the for loop's index starts at. */
            if (!vm_push(py_int(0)))
                goto fail;
            break;

        case OP_PUSH_TRUE:
            if (!vm_push(py_bool(1)))
                goto fail;
            break;

        case OP_PUSH_FALSE:
            if (!vm_push(py_bool(0)))
                goto fail;
            break;

        case OP_DUP: {
            if (!vsp) {
                vm_fail(line, "InternalError: the stack is empty");
                goto fail;
            }
            py_value v = vstack[vsp - 1];
            py_incref(v);
            if (!vm_push(v))
                goto fail;
            break;
        }

        case OP_POP: {
            py_value v = vm_pop();
            py_decref(v);
            break;
        }

        case OP_LOAD: {
            const char *name = prog->names[b];
            py_value v;
            if (!py_scope_lookup(scope, name, &v)) {
                rt_error5(line, "NameError: name '", name, "' is not defined",
                          0, 0);
                vm_fail(line, "");
                goto fail;
            }
            py_incref(v);
            if (!vm_push(v))
                goto fail;
            break;
        }

        case OP_STORE: {
            /* The value is on the stack; the scope takes it over. */
            py_value v = vm_pop();
            py_scope_assign(scope, prog->names[b], v);
            break;
        }

        case OP_DEFINE: {
            py_value v = vm_pop();
            py_scope_define(scope, prog->names[b], v);
            break;
        }

        case OP_DELETE: {
            const char *name = prog->names[b];
            if (!py_scope_delete(scope, name)) {
                rt_error5(line, "NameError: name '", name, "' is not defined",
                          0, 0);
                vm_fail(line, "");
                goto fail;
            }
            break;
        }

        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD: {
            py_value rhs = vm_pop();
            py_value lhs = vm_pop();
            py_value r;
            switch (op) {
            case OP_ADD: r = py_op_add(lhs, rhs, line); break;
            case OP_SUB: r = py_op_sub(lhs, rhs, line); break;
            case OP_MUL: r = py_op_mul(lhs, rhs, line); break;
            case OP_DIV: r = py_op_div(lhs, rhs, line); break;
            default:     r = py_op_mod(lhs, rhs, line); break;
            }
            py_decref(lhs);
            py_decref(rhs);
            if (vm_err)
                goto fail;
            if (!vm_push(r))
                goto fail;
            break;
        }

        case OP_NEG: {
            py_value v = vm_pop();
            py_value r = py_op_neg(v, line);
            py_decref(v);
            if (vm_err)
                goto fail;
            if (!vm_push(r))
                goto fail;
            break;
        }

        case OP_NOT: {
            py_value v = vm_pop();
            py_value r = py_bool(!py_truthy(v));
            py_decref(v);
            if (!vm_push(r))
                goto fail;
            break;
        }

        case OP_EQ:
        case OP_NE:
        case OP_LT:
        case OP_LE:
        case OP_GT:
        case OP_GE: {
            py_value rhs = vm_pop();
            py_value lhs = vm_pop();
            int c;
            switch (op) {
            case OP_EQ: c = py_op_eq(lhs, rhs); break;
            case OP_NE: c = py_op_ne(lhs, rhs); break;
            case OP_LT: c = py_op_lt(lhs, rhs, line); break;
            case OP_LE: c = py_op_le(lhs, rhs, line); break;
            case OP_GT: c = py_op_gt(lhs, rhs, line); break;
            default:     c = py_op_ge(lhs, rhs, line); break;
            }
            py_decref(lhs);
            py_decref(rhs);
            if (vm_err)
                goto fail;
            if (!vm_push(py_bool(c)))
                goto fail;
            break;
        }

        case OP_IN: {
            py_value container = vm_pop();
            py_value item = vm_pop();
            int c = py_op_in(item, container, line);
            py_decref(item);
            py_decref(container);
            if (vm_err)
                goto fail;
            if (!vm_push(py_bool(c)))
                goto fail;
            break;
        }

        case OP_JUMP: {
            /* The distance is relative to the instruction after the
             * jump, and it is signed. */
            int32_t dist = (int32_t)(int16_t)b;
            pc = (uint32_t)((int32_t)pc + dist);
            break;
        }

        case OP_JUMP_IF_FALSE:
        case OP_JUMP_IF_TRUE: {
            py_value v = vm_pop();
            int t = py_truthy(v);
            py_decref(v);
            if ((op == OP_JUMP_IF_FALSE && !t) || (op == OP_JUMP_IF_TRUE && t)) {
                int32_t dist = (int32_t)(int16_t)b;
                pc = (uint32_t)((int32_t)pc + dist);
            }
            break;
        }

        case OP_MAKE_LIST: {
            /* The elements are on the stack, in order. */
            uint32_t n = a;
            py_value l = py_new_list();
            if (l.type != PY_LIST) {
                vm_fail(line, "MemoryError: out of memory");
                goto fail;
            }
            for (uint32_t i = n; i > 0; i--) {
                py_value v = vstack[vsp - i];
                py_incref(v);
                py_list_append(l, v);
            }
            vsp -= n;
            if (!vm_push(l))
                goto fail;
            break;
        }

        case OP_MAKE_DICT: {
            /* The keys and the values are on the stack, alternating. */
            uint32_t n = a;
            py_value d = py_new_dict();
            if (d.type != PY_DICT) {
                vm_fail(line, "MemoryError: out of memory");
                goto fail;
            }
            for (uint32_t i = n; i > 0; i--) {
                py_value v = vstack[vsp - i * 2 + 1];
                py_value k = vstack[vsp - i * 2];
                py_incref(k);
                py_incref(v);
                py_dict_set(d, k, v);
                py_decref(k);
                py_decref(v);
            }
            vsp -= n * 2;
            if (!vm_push(d))
                goto fail;
            break;
        }

        case OP_GET_ITEM: {
            py_value idx = vm_pop();
            py_value cont = vm_pop();
            py_value r;
            if (!vm_index(cont, idx, line, &r))
                goto fail;
            py_decref(cont);
            py_decref(idx);
            if (!vm_push(r))
                goto fail;
            break;
        }

        case OP_SET_ITEM: {
            /* The value, the container and the index are on the stack:
             * the compiler pushed the value first, because Python
             * evaluates the right hand side before the target. */
            py_value idx = vm_pop();
            py_value cont = vm_pop();
            py_value val = vm_pop();
            if (!vm_set_item(cont, idx, val, line)) {
                py_decref(cont);
                py_decref(idx);
                py_decref(val);
                goto fail;
            }
            py_decref(cont);
            py_decref(idx);
            py_decref(val);
            break;
        }

        case OP_DEL_ITEM: {
            py_value idx = vm_pop();
            py_value cont = vm_pop();
            if (!vm_del_item(cont, idx, line)) {
                py_decref(cont);
                py_decref(idx);
                goto fail;
            }
            py_decref(cont);
            py_decref(idx);
            break;
        }

        case OP_MAKE_FUNC: {
            py_value f = make_function(prog, b, scope, prog->names[b]);
            if (!f.u.obj) {
                vm_fail(line, "MemoryError: out of memory");
                goto fail;
            }
            if (!vm_push(f))
                goto fail;
            break;
        }

        case OP_FOR_PREP: {
            /* The iterable is on the stack; it becomes a list of items,
             * which the loop walks. */
            py_value it = vm_pop();
            py_value items = py_iterable_items(it, line);
            py_decref(it);
            if (vm_err)
                goto fail;
            if (!vm_push(items))
                goto fail;
            break;
        }

        case OP_FOR_NEXT: {
            /* The loop's own two slots are on the stack: the list of
             * items, and the index into it.  The list was pushed first,
             * so the index is on top. */
            if (vsp < 2) {
                vm_fail(line, "InternalError: the stack is empty");
                goto fail;
            }
            py_value items = vstack[vsp - 2];
            if (items.type != PY_LIST) {
                vm_fail(line, "InternalError: the loop is not a list");
                goto fail;
            }
            py_list *l = &items.u.obj->u.list;
            uint32_t idx = (uint32_t)vstack[vsp - 1].u.i;
            if (idx >= l->len) {
                /* No more items: the loop ends.  The two slots are
                 * dropped by the OP_POP_ITERS the compiler put at the
                 * end of the loop. */
                int32_t dist = (int32_t)(int16_t)b;
                pc = (uint32_t)((int32_t)pc + dist);
                break;
            }
            py_value item = l->items[idx];
            py_incref(item);
            vstack[vsp - 1].u.i = (int32_t)(idx + 1);
            if (!vm_push(item))
                goto fail;
            break;
        }

        case OP_POP_ITERS: {
            /* The loop's index and its list of items are dropped. */
            if (vsp >= 2) {
                py_value idx = vstack[vsp - 1];
                py_value items = vstack[vsp - 2];
                py_decref(idx);
                if (items.type == PY_LIST)
                    py_decref(items);
                vsp -= 2;
            }
            break;
        }

        case OP_RETURN: {
            py_value v = vm_pop();
            *out = v;
            return 0;
        }

        case OP_CALL: {
            py_value r;
            if (!vm_call(prog, scope, a, 0, 0, line, &r))
                goto fail;
            if (!vm_push(r))
                goto fail;
            break;
        }

        case OP_CALL_KW: {
            /* The plan says how many arguments are positional and what
             * the keyword arguments are called. */
            uint32_t plan = b;
            uint32_t npos = prog->plans[plan];
            uint32_t nkw = (uint32_t)a - npos;
            py_value r;
            if (!vm_call(prog, scope, (int)npos, (int)nkw, plan + 1, line, &r))
                goto fail;
            if (!vm_push(r))
                goto fail;
            break;
        }

        case OP_CALL_METHOD: {
            py_value r;
            if (!vm_call_method(prog, scope, a, b, line, &r))
                goto fail;
            if (!vm_push(r))
                goto fail;
            break;
        }

        case OP_GET_ATTR: {
            /* An attribute that is not called is an error: this
             * interpreter has no bound methods. */
            py_value obj = vm_pop();
            py_decref(obj);
            rt_error5(line, "AttributeError: attributes can only be used in "
                      "a method call", 0, 0, 0, 0);
            vm_fail(line, "");
            goto fail;
        }

        default:
            vm_fail(line, "InternalError: unknown instruction");
            goto fail;
        }
    }

    *out = py_none();
    return 0;

fail:
    *out = py_none();
    return -1;
}

/* ---- calls --------------------------------------------------------------- */

/* A call.
 *
 * The stack holds the callee and the arguments:
 *
 *     <callee>  <arg0> ... <argN-1>
 *
 * with the arguments on top.  A builtin is called through its C
 * function; a user function gets a scope of its own, the arguments are
 * bound to the parameters, and its code runs.
 *
 * `nkw` keyword arguments follow the positional ones, and the plan
 * holds their names. */
static int vm_call(py_prog *prog, py_scope *scope, int argc, int nkw,
                   uint32_t plan, uint32_t line, py_value *out)
{
    uint32_t total = (uint32_t)argc + (uint32_t)nkw;
    if (vsp < total + 1) {
        vm_fail(line, "InternalError: the stack is empty");
        return 0;
    }

    py_value callee = vstack[vsp - total - 1];
    py_value *args = &vstack[vsp - total];

    if (callee.type == PY_BUILTIN) {
        /* The keyword argument names come from the plan. */
        char *kwnames[16];
        for (int k = 0; k < nkw && k < 16; k++)
            kwnames[k] = prog->names[prog->plans[plan + k]];

        struct py_call call;
        call.args = args;
        call.argc = argc;
        call.kwvals = nkw ? &args[argc] : 0;
        call.kwnames = nkw ? kwnames : 0;
        call.nkwargs = nkw;

        py_value r = callee.u.obj->u.builtin.fn(&call);

        /* The arguments are the caller's to release. */
        for (uint32_t i = 0; i < total; i++)
            py_decref(args[i]);
        py_decref(callee);
        vsp -= total + 1;

        if (vm_err)
            return 0;
        *out = r;
        return 1;
    }

    if (callee.type == PY_FUNC) {
        py_func *f = &callee.u.obj->u.func;
        const py_code *fc = prog->codes[f->code];

        /* The names of the keyword arguments come from the plan, which
         * holds the indexes of the names in the program's name table.
         * The indexes are not consecutive, so the names are collected
         * into a list first. */
        char *kwnames[16];
        for (int k = 0; k < nkw && k < 16; k++)
            kwnames[k] = prog->names[prog->plans[plan + k]];

        /* The scope of the call: the function's own, with the scope it
         * was defined in as its parent.  That parent is what makes a
         * closure work. */
        py_scope *fscope = py_scope_new(f->closure);
        if (!fscope) {
            vm_fail(line, "MemoryError: out of memory");
            return 0;
        }

        if (!bind_params(prog, fc, fscope, args, argc, kwnames, nkw, line)) {
            /* The scope holds whatever it managed to bind; it goes
             * with the call. */
            py_scope_release(fscope);
            vm_fail(line, "");
            return 0;
        }

        /* The arguments are the caller's to release. */
        for (uint32_t i = 0; i < total; i++)
            py_decref(args[i]);
        py_decref(callee);
        vsp -= total + 1;

        py_value r;
        if (vm_run(prog, fc, fscope, &r) != 0)
            return 0;

        /* The scope of the call goes away with it. */
        py_scope_release(fscope);

        *out = r;
        return 1;
    }

    vm_fail(line, "TypeError: object is not callable");
    return 0;
}

/* A method call: the object and the arguments are on the stack, and the
 * method is found by name in the table for the object's type. */
static int vm_call_method(py_prog *prog, py_scope *scope, int argc, uint32_t nameidx,
                          uint32_t line, py_value *out)
{
    if (vsp < (uint32_t)argc + 1) {
        vm_fail(line, "InternalError: the stack is empty");
        return 0;
    }

    py_value obj = vstack[vsp - argc - 1];
    py_value *args = &vstack[vsp - argc];

    py_value r;
    if (!py_call_method(obj, prog->names[nameidx], args, argc, line, &r)) {
        vm_fail(line, "");
        return 0;
    }

    for (int i = 0; i < argc; i++)
        py_decref(args[i]);
    py_decref(obj);
    vsp -= (uint32_t)argc + 1;

    *out = r;
    return 1;
}

/* ---- running a program ---------------------------------------------------- */

/* Run a compiled program.  Returns 0 on success. */
int py_vm_run(py_prog *prog)
{
    vm_err = 0;
    vm_err_line = 0;
    py_rt_error_clear();

    /* The module's scope, with the builtins in it.  They are ordinary
     * names, so a script can shadow them. */
    py_scope *module = py_scope_new(0);
    if (!module) {
        vm_fail(0, "MemoryError: out of memory");
        return -1;
    }
    {
        /* The builtins go in under the names the program's name table
         * holds, so that a script's own `print` is the same name. */
        const char *names[32];
        uint32_t n = prog->nbuiltin_names;
        if (n > 32)
            n = 32;
        for (uint32_t i = 0; i < n; i++)
            names[i] = prog->names[prog->builtin_names[i]];
        py_scope_define_builtins(module, names);
    }

    py_value r;
    int rc = vm_run(prog, prog->codes[0], module, &r);
    py_decref(r);
    py_scope_release(module);

    if (rc != 0)
        return -1;
    return 0;
}
