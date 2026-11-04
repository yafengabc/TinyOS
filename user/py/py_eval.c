/* py_eval.c - the evaluator.
 *
 * Walks the tree the parser built and does what it says.  Three pieces:
 * an environment (the names a scope can see), expression evaluation, and
 * statement execution.
 *
 * Scopes are lexical.  A function body sees the names of the scope it was
 * defined in, and a name is looked up outward from where it is used.
 *
 * One rule differs from Python on purpose.  Python makes every name a
 * function assigns to local to that function, so touching a global from a
 * function needs a `global` statement.  This subset has no `global`, so
 * the rule is: assign where the name is already bound, searching outward;
 * otherwise define it in the current scope.  That makes the counter idiom
 * work without one, at the cost of Python's "UnboundLocalError".
 *
 * The other difference is arithmetic: there is no floating point here (the
 * three targets have no FPU, and a 64-bit divide does not even link), so
 * `/` is integer division and the numbers are 32-bit.
 */

#include "py_eval.h"
#include "py_mem.h"
#include "py_str.h"
#include "lib.h"

/* ---- errors ------------------------------------------------------------ */

/* The error message is built in one buffer, so a name or a type can be
 * woven into it.  The first error wins: a failed expression leaves the
 * rest of the program unevaluated, and the one that failed is the one
 * worth reporting. */
#define EV_MAX 200
static char ev_buf[EV_MAX];
static const char *ev_err;
static uint32_t ev_line;

static char *app(char *w, char *end, const char *s)
{
    while (*s && w < end - 1)
        *w++ = *s++;
    return w;
}

static void ev_setn(uint32_t line, const char *a, const char *b, const char *c,
                    const char *d, const char *e)
{
    if (ev_err)
        return;
    char *w = ev_buf;
    char *end = ev_buf + EV_MAX;
    w = app(w, end, a);
    w = app(w, end, b);
    w = app(w, end, c);
    w = app(w, end, d);
    w = app(w, end, e);
    *w = 0;
    ev_err = ev_buf;
    ev_line = line;
}

/* Set when an allocation inside the evaluator failed.  The failure has to
 * surface as "out of memory" rather than as whatever the missing value
 * looks like downstream - a scope that could not be built looks exactly
 * like a function that was called with the wrong arguments. */
static int ev_oom;

const char *py_eval_error(void) { return ev_err; }
uint32_t py_eval_line(void) { return ev_line; }

/* ---- environment -------------------------------------------------------- */

/* A scope is a hash table of names.  The keys are C strings that live in
 * the interpreter's arena for the rest of the run (they come from the
 * syntax tree, or from a string the program built), so the table holds
 * pointers, not copies. */
typedef struct binding {
    char *name;
    py_value val;
    uint8_t dead;        /* deleted by `del`: the slot stays, the name does not */
} binding;

struct py_env {
    binding *slots;
    uint32_t cap;
    uint32_t count;
    py_env *parent;
    py_value ret;        /* what a `return` in this scope set */
};

static uint32_t hash_name(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 16777619u;
    }
    return h;
}

static py_env *env_new(py_env *parent)
{
    py_env *e = (py_env *)py_alloc((uint32_t)sizeof(py_env));
    if (!e) {
        ev_oom = 1;
        return 0;
    }
    e->slots = 0;
    e->cap = 0;
    e->count = 0;
    e->parent = parent;
    e->ret = py_none();
    return e;
}

static int env_grow(py_env *e)
{
    /* Two slots to start.  Every call opens a table, and a function with
     * one parameter is the common case; the table doubles when it needs
     * to, so this costs a rehash later and saves a block now. */
    uint32_t ncap = e->cap ? e->cap * 2 : 2;
    binding *ns = (binding *)py_alloc(ncap * (uint32_t)sizeof(binding));
    if (!ns)
        return -1;
    for (uint32_t i = 0; i < ncap; i++) {
        ns[i].name = 0;
        ns[i].val = py_none();
        ns[i].dead = 0;
    }
    for (uint32_t i = 0; i < e->cap; i++) {
        if (!e->slots[i].name)
            continue;
        uint32_t j = hash_name(e->slots[i].name) & (ncap - 1);
        while (ns[j].name)
            j = (j + 1) & (ncap - 1);
        ns[j] = e->slots[i];
    }
    if (e->slots)
        py_free_auto(e->slots);
    e->slots = ns;
    e->cap = ncap;
    return 0;
}

/* The slot that holds this name, whether the name is live or was deleted.
 * A deleted name keeps its slot so the probe sequence stays intact, and so
 * that assigning to it again revives it rather than making a second one. */
static binding *env_find_slot(py_env *e, const char *name)
{
    if (!e->cap)
        return 0;
    uint32_t mask = e->cap - 1;
    uint32_t i = hash_name(name) & mask;
    while (e->slots[i].name) {
        if (!strcmp(e->slots[i].name, name))
            return &e->slots[i];
        i = (i + 1) & mask;
    }
    return 0;
}

/* Define a name in this scope.  Takes over the reference to `v`. */
static int env_define(py_env *e, const char *name, py_value v)
{
    binding *b = env_find_slot(e, name);
    if (b) {
        py_decref(b->val);
        b->val = v;
        if (b->dead) {
            b->dead = 0;
            e->count++;
        }
        return 0;
    }
    if ((e->count + 1) * 4 >= e->cap * 3) {
        if (env_grow(e) != 0) {
            ev_oom = 1;
            return -1;
        }
    }
    uint32_t mask = e->cap - 1;
    uint32_t i = hash_name(name) & mask;
    while (e->slots[i].name)
        i = (i + 1) & mask;
    e->slots[i].name = (char *)name;
    e->slots[i].val = v;
    e->count++;
    return 0;
}

/* Look a name up, outward from this scope.  The result is borrowed: the
 * caller increfs it if it keeps it. */
static int env_lookup(py_env *e, const char *name, py_value *out)
{
    for (py_env *p = e; p; p = p->parent) {
        binding *b = env_find_slot(p, name);
        if (b) {
            /* A deleted name is not bound: the search stops here rather
             * than reaching for an outer one, the way a local variable
             * that was deleted is still local. */
            if (b->dead)
                return 0;
            *out = b->val;
            return 1;
        }
    }
    return 0;
}

/* Is this name bound here?  A deleted name is not. */
static int env_is_bound(py_env *e, const char *name)
{
    binding *b = env_find_slot(e, name);
    return b && !b->dead;
}

/* Remove a name from this scope.  Returns 0 when there was no such name. */
static int env_del(py_env *e, const char *name)
{
    binding *b = env_find_slot(e, name);
    if (!b || b->dead)
        return 0;
    py_decref(b->val);
    b->val = py_none();
    b->dead = 1;
    e->count--;
    return 1;
}

/* Assign to a name: update the binding where it already is, or define it
 * here.  Takes over the reference to `v`. */
static int env_assign(py_env *e, const char *name, py_value v)
{
    for (py_env *p = e; p; p = p->parent) {
        binding *b = env_find_slot(p, name);
        if (b) {
            py_decref(b->val);
            b->val = v;
            if (b->dead) {
                b->dead = 0;
                p->count++;
            }
            return 0;
        }
    }
    return env_define(e, name, v);
}

/* ---- forward declarations ----------------------------------------------- */

typedef enum {
    FLOW_NORMAL = 0,
    FLOW_BREAK,
    FLOW_CONTINUE,
    FLOW_RETURN
} py_flow;

static py_value eval_node(py_node *n, py_env *env);
static py_flow exec_stmt(py_node *n, py_env *env);
static py_flow exec_block(py_node *first, py_env *env);
static py_flow exec_suite(py_node *suite, py_env *env);

/* ---- small helpers ------------------------------------------------------ */

static int as_int(py_value v, int32_t *out)
{
    if (v.type == PY_INT) {
        *out = v.u.i;
        return 1;
    }
    if (v.type == PY_BOOL) {
        *out = v.u.i ? 1 : 0;
        return 1;
    }
    return 0;
}

static void out_str(const char *s)
{
    if (s)
        write(1, s, (int)strlen(s));
}

static void out_bytes(const char *s, uint32_t n)
{
    if (s && n)
        write(1, s, (int)n);
}

/* Build a string value from two pieces.  py_new_strn() allocates the bytes
 * with the object, so the pieces are copied in afterwards. */
static py_value str_concat(py_str *a, py_str *b)
{
    uint32_t total = a->len + b->len;
    py_value r = py_new_strn(0, total);
    if (r.type != PY_STR)
        return r;
    if (a->len)
        memcpy(r.u.obj->u.str.bytes, a->bytes, a->len);
    if (b->len)
        memcpy(r.u.obj->u.str.bytes + a->len, b->bytes, b->len);
    return r;
}

/* ---- operators ---------------------------------------------------------- */

static py_value binop(uint8_t op, py_value a, py_value b, uint32_t line)
{
    int32_t x = 0, y = 0;
    int ai = as_int(a, &x);
    int bi = as_int(b, &y);

    switch (op) {
    case PY_OP_ADD:
        if (ai && bi)
            return py_int(x + y);
        if (a.type == PY_STR && b.type == PY_STR)
            return str_concat(&a.u.obj->u.str, &b.u.obj->u.str);
        if (a.type == PY_LIST && b.type == PY_LIST) {
            py_value l = py_new_list();
            py_list *la = &a.u.obj->u.list;
            py_list *lb = &b.u.obj->u.list;
            for (uint32_t i = 0; i < la->len; i++) {
                py_value it = la->items[i];
                py_incref(it);
                py_list_append(l, it);
            }
            for (uint32_t i = 0; i < lb->len; i++) {
                py_value it = lb->items[i];
                py_incref(it);
                py_list_append(l, it);
            }
            return l;
        }
        break;

    case PY_OP_SUB:
        if (ai && bi)
            return py_int(x - y);
        break;

    case PY_OP_MUL:
        if (ai && bi)
            return py_int(x * y);
        if (a.type == PY_STR && bi) {
            if (y <= 0)
                return py_new_str("");
            py_str *s = &a.u.obj->u.str;
            py_value r = py_new_strn(0, s->len * (uint32_t)y);
            if (r.type != PY_STR)
                return r;
            for (int32_t i = 0; i < y; i++)
                memcpy(r.u.obj->u.str.bytes + (uint32_t)i * s->len, s->bytes, s->len);
            return r;
        }
        if (a.type == PY_LIST && bi) {
            if (y <= 0)
                return py_new_list();
            py_list *l = &a.u.obj->u.list;
            py_value r = py_new_list();
            for (int32_t i = 0; i < y; i++) {
                for (uint32_t j = 0; j < l->len; j++) {
                    py_value it = l->items[j];
                    py_incref(it);
                    py_list_append(r, it);
                }
            }
            return r;
        }
        if (bi && a.type == PY_STR)
            return binop(op, b, a, line);
        break;

    case PY_OP_DIV:
        if (ai && bi) {
            if (y == 0) {
                ev_setn(line, "ZeroDivisionError: division by zero", 0, 0, 0, 0);
                return py_none();
            }
            /* Integer division, the way C truncates towards zero.  Python's
             * `/` is true division; there are no floats here, so this is
             * what `/` means in this subset. */
            return py_int(x / y);
        }
        break;

    case PY_OP_MOD:
        if (ai && bi) {
            if (y == 0) {
                ev_setn(line, "ZeroDivisionError: modulo by zero", 0, 0, 0, 0);
                return py_none();
            }
            int32_t r = x % y;
            /* Python's modulo takes the sign of the divisor; C's takes the
             * sign of the dividend. */
            if (r != 0 && ((r < 0) != (y < 0)))
                r += y;
            return py_int(r);
        }
        break;
    }

    ev_setn(line, "TypeError: unsupported operand type(s) for ", 0, 0, 0, 0);
    return py_none();
}

static int value_in(py_value item, py_value container)
{
    if (container.type == PY_LIST) {
        py_list *l = &container.u.obj->u.list;
        for (uint32_t i = 0; i < l->len; i++) {
            if (py_eq(l->items[i], item))
                return 1;
        }
        return 0;
    }
    if (container.type == PY_DICT) {
        py_value out;
        return py_dict_get(container, item, &out);
    }
    if (container.type == PY_STR) {
        if (item.type != PY_STR)
            return 0;
        py_str *h = &container.u.obj->u.str;
        py_str *n = &item.u.obj->u.str;
        if (n->len == 0)
            return 1;
        if (n->len > h->len)
            return 0;
        for (uint32_t i = 0; i + n->len <= h->len; i++) {
            if (!memcmp(h->bytes + i, n->bytes, n->len))
                return 1;
        }
        return 0;
    }
    return 0;
}

static py_value compare_op(uint8_t op, py_value a, py_value b, uint32_t line)
{
    if (op == PY_OP_EQ)
        return py_bool(py_eq(a, b));
    if (op == PY_OP_NE)
        return py_bool(!py_eq(a, b));

    int32_t x = 0, y = 0;
    if (as_int(a, &x) && as_int(b, &y)) {
        switch (op) {
        case PY_OP_LT: return py_bool(x < y);
        case PY_OP_LE: return py_bool(x <= y);
        case PY_OP_GT: return py_bool(x > y);
        case PY_OP_GE: return py_bool(x >= y);
        }
    }
    if (a.type == PY_STR && b.type == PY_STR) {
        py_str *sa = &a.u.obj->u.str;
        py_str *sb = &b.u.obj->u.str;
        uint32_t n = sa->len < sb->len ? sa->len : sb->len;
        int c = n ? memcmp(sa->bytes, sb->bytes, n) : 0;
        int cmp = c ? c : (sa->len < sb->len ? -1 : (sa->len > sb->len ? 1 : 0));
        switch (op) {
        case PY_OP_LT: return py_bool(cmp < 0);
        case PY_OP_LE: return py_bool(cmp <= 0);
        case PY_OP_GT: return py_bool(cmp > 0);
        case PY_OP_GE: return py_bool(cmp >= 0);
        }
    }
    ev_setn(line, "TypeError: '", py_type_name(a), "' not supported between "
                  "instances of '", py_type_name(b), "'");
    return py_none();
}

/* ---- containers as iterables -------------------------------------------- */

/* The items an iterable yields, as a fresh list.  Copying the list first
 * means a loop body that appends to the list it is iterating does not
 * change the loop underfoot. */
static py_value iterable_items(py_value v, uint32_t line)
{
    py_value l = py_new_list();

    if (v.type == PY_LIST) {
        py_list *src = &v.u.obj->u.list;
        for (uint32_t i = 0; i < src->len; i++) {
            py_value it = src->items[i];
            py_incref(it);
            py_list_append(l, it);
        }
        return l;
    }
    if (v.type == PY_DICT) {
        py_dict *d = &v.u.obj->u.dict;
        for (uint32_t i = 0; i < d->count; i++) {
            py_value k = d->entries[i].key;
            py_incref(k);
            py_list_append(l, k);
        }
        return l;
    }
    if (v.type == PY_STR) {
        py_str *s = &v.u.obj->u.str;
        for (uint32_t i = 0; i < s->len; i++)
            py_list_append(l, py_new_strn(s->bytes + i, 1));
        return l;
    }

    ev_setn(line, "TypeError: '", py_type_name(v), "' is not iterable", 0, 0);
    py_decref(l);
    return py_none();
}

/* ---- calls -------------------------------------------------------------- */

typedef struct {
    py_value *args;
    int argc;
    py_value *kwvals;
    char **kwnames;
    int nkwargs;
} arglist;

/* A keyword argument was parsed into a BINOP node with no operator and a
 * name - see parse_postfix(). */
static int is_kwarg(py_node *a)
{
    return a->kind == PY_E_BINOP && a->op == PY_OP_NONE && a->name != 0;
}

static int collect_args(py_node *callnode, py_env *env, arglist *out)
{
    /* The fields are cleared first: a failure below returns early, and
     * the caller frees the list whatever happened.  A list that was never
     * filled in would free a pointer that was never a pointer. */
    out->args = 0;
    out->argc = 0;
    out->kwvals = 0;
    out->kwnames = 0;
    out->nkwargs = 0;

    uint32_t npos = 0, nkw = 0;
    for (py_node *a = callnode->u.call.args; a; a = a->next) {
        if (is_kwarg(a))
            nkw++;
        else
            npos++;
    }

    /* One block for all three arrays.  A call happens often enough that
     * three separate allocations cost more in rounding than the space
     * they save. */
    uint32_t asz = (uint32_t)sizeof(py_value);
    uint32_t psz = (uint32_t)sizeof(char *);
    uint32_t bytes = npos * asz + nkw * asz + nkw * psz;
    if (!bytes)
        bytes = asz;
    char *blk = (char *)py_alloc(bytes);
    if (!blk) {
        ev_setn(callnode->line, "MemoryError: out of memory", 0, 0, 0, 0);
        return -1;
    }
    out->args = (py_value *)blk;
    out->kwvals = (py_value *)(blk + npos * asz);
    out->kwnames = (char **)(blk + npos * asz + nkw * asz);
    out->argc = 0;
    out->nkwargs = 0;

    for (py_node *a = callnode->u.call.args; a; a = a->next) {
        py_value v = eval_node(is_kwarg(a) ? a->a : a, env);
        if (ev_err)
            return -1;
        if (is_kwarg(a)) {
            out->kwvals[out->nkwargs] = v;
            out->kwnames[out->nkwargs] = a->name;
            out->nkwargs++;
        } else {
            out->args[out->argc++] = v;
        }
    }
    return 0;
}

static void free_args(arglist *al)
{
    for (int i = 0; i < al->argc; i++)
        py_decref(al->args[i]);
    for (int i = 0; i < al->nkwargs; i++)
        py_decref(al->kwvals[i]);
    /* One block holds all three arrays; see collect_args(). */
    if (al->args)
        py_free_auto(al->args);
}

/* A user defined function.  The body runs in a scope of its own whose
 * parent is the scope the def appeared in. */
static py_value call_function(py_obj *fnobj, py_node *callnode, py_env *env)
{
    py_func *f = &fnobj->u.func;
    uint32_t np = f->def->u.fn.nparams;

    arglist al;
    if (collect_args(callnode, env, &al) != 0) {
        free_args(&al);
        return py_none();
    }

    if ((uint32_t)al.argc > np) {
        ev_setn(callnode->line, "TypeError: ", f->name,
                "() takes " , 0, 0);
        free_args(&al);
        return py_none();
    }

    py_env *fenv = env_new(f->closure);
    if (!fenv) {
        ev_setn(callnode->line, "MemoryError: out of memory", 0, 0, 0, 0);
        free_args(&al);
        return py_none();
    }

    /* Positional arguments bind left to right. */
    for (uint32_t i = 0; i < (uint32_t)al.argc; i++)
        env_define(fenv, f->def->u.fn.params[i], al.args[i]);
    al.argc = 0;   /* the scope owns them now */

    /* Keyword arguments bind by name. */
    for (int k = 0; k < al.nkwargs; k++) {
        int found = -1;
        for (uint32_t i = 0; i < np; i++) {
            if (!strcmp(f->def->u.fn.params[i], al.kwnames[k])) {
                found = (int)i;
                break;
            }
        }
        if (found < 0) {
            ev_setn(callnode->line, "TypeError: ", f->name,
                    "() got an unexpected keyword argument", 0, 0);
            goto fail;
        }
        if (env_is_bound(fenv, al.kwnames[k])) {
            ev_setn(callnode->line, "TypeError: ", f->name,
                    "() got multiple values for argument", 0, 0);
            goto fail;
        }
        env_define(fenv, al.kwnames[k], al.kwvals[k]);
    }
    al.nkwargs = 0;  /* the scope owns them now */

    /* Every parameter has to be bound. */
    for (uint32_t i = 0; i < np; i++) {
        if (!env_is_bound(fenv, f->def->u.fn.params[i])) {
            ev_setn(callnode->line, "TypeError: ", f->name,
                    "() missing a required argument", 0, 0);
            goto fail;
        }
    }

    exec_suite(f->body->body, fenv);
    if (ev_err) {
        py_decref(fenv->ret);
        fenv->ret = py_none();
        return py_none();
    }

    {
        py_value ret = fenv->ret;
        fenv->ret = py_none();
        return ret;
    }

fail:
    free_args(&al);
    return py_none();
}

/* ---- methods ------------------------------------------------------------ */

/* A method gets the object it was called on as its first argument.  The
 * name is matched against the table for the object's type. */
typedef py_value (*method_fn)(py_value self, py_value *args, int argc, uint32_t line);

typedef struct {
    const char *name;
    method_fn fn;
} method_entry;

static py_value bi_print(struct py_call *c);

static const method_entry *find_method(py_value obj, const char *name);

static py_value call_method(py_value obj, const char *name, py_node *callnode,
                            py_env *env)
{
    const method_entry *m = find_method(obj, name);
    if (!m) {
        ev_setn(callnode->line, "AttributeError: '", py_type_name(obj),
                "' object has no attribute '", name, "'");
        return py_none();
    }

    arglist al;
    if (collect_args(callnode, env, &al) != 0) {
        free_args(&al);
        return py_none();
    }
    if (al.nkwargs) {
        ev_setn(callnode->line, "TypeError: methods do not take keyword "
                                "arguments in this subset", 0, 0, 0, 0);
        free_args(&al);
        return py_none();
    }

    py_value r = m->fn(obj, al.args, al.argc, callnode->line);
    free_args(&al);
    return r;
}

/* list methods */

static py_value m_list_append(py_value self, py_value *args, int argc, uint32_t line)
{
    if (argc != 1) {
        ev_setn(line, "TypeError: append() takes exactly one argument", 0, 0, 0, 0);
        return py_none();
    }
    /* The list takes over a reference of its own, and free_args() will
     * release the caller's one - so the value needs a second reference
     * before it goes in, or it would be released twice. */
    py_incref(args[0]);
    py_list_append(self, args[0]);
    return py_none();
}

static py_value m_list_pop(py_value self, py_value *args, int argc, uint32_t line)
{
    int32_t idx = -1;
    if (argc > 1) {
        ev_setn(line, "TypeError: pop() takes at most one argument", 0, 0, 0, 0);
        return py_none();
    }
    if (argc == 1 && !as_int(args[0], &idx)) {
        ev_setn(line, "TypeError: pop() index must be an int", 0, 0, 0, 0);
        return py_none();
    }
    int32_t len = py_list_len(self);
    int32_t i = idx;
    if (i < 0)
        i += len;
    if (i < 0 || i >= len) {
        ev_setn(line, "IndexError: pop index out of range", 0, 0, 0, 0);
        return py_none();
    }
    py_value v = py_list_pop(self, i);
    /* py_list_pop hands the list's reference over, so this one is already
     * ours - adding another would leak it. */
    return v;
}

static py_value m_list_insert(py_value self, py_value *args, int argc, uint32_t line)
{
    if (argc != 2) {
        ev_setn(line, "TypeError: insert() takes exactly two arguments", 0, 0, 0, 0);
        return py_none();
    }
    int32_t idx = 0;
    if (!as_int(args[0], &idx)) {
        ev_setn(line, "TypeError: insert() index must be an int", 0, 0, 0, 0);
        return py_none();
    }
    py_incref(args[1]);
    py_list_insert(self, idx, args[1]);
    return py_none();
}

static py_value m_list_remove(py_value self, py_value *args, int argc, uint32_t line)
{
    if (argc != 1) {
        ev_setn(line, "TypeError: remove() takes exactly one argument", 0, 0, 0, 0);
        return py_none();
    }
    py_list *l = &self.u.obj->u.list;
    for (uint32_t i = 0; i < l->len; i++) {
        if (!py_eq(l->items[i], args[0]))
            continue;
        py_decref(l->items[i]);
        for (uint32_t j = i; j + 1 < l->len; j++)
            l->items[j] = l->items[j + 1];
        l->len--;
        return py_none();
    }
    ev_setn(line, "ValueError: list.remove(x): x not in list", 0, 0, 0, 0);
    return py_none();
}

static py_value m_list_index(py_value self, py_value *args, int argc, uint32_t line)
{
    if (argc != 1) {
        ev_setn(line, "TypeError: index() takes exactly one argument", 0, 0, 0, 0);
        return py_none();
    }
    py_list *l = &self.u.obj->u.list;
    for (uint32_t i = 0; i < l->len; i++) {
        if (py_eq(l->items[i], args[0]))
            return py_int((int32_t)i);
    }
    ev_setn(line, "ValueError: value is not in list", 0, 0, 0, 0);
    return py_none();
}

static py_value m_list_clear(py_value self, py_value *args, int argc, uint32_t line)
{
    py_list *l = &self.u.obj->u.list;
    for (uint32_t i = 0; i < l->len; i++)
        py_decref(l->items[i]);
    l->len = 0;
    return py_none();
}

static const method_entry list_methods[] = {
    { "append", m_list_append },
    { "pop", m_list_pop },
    { "insert", m_list_insert },
    { "remove", m_list_remove },
    { "index", m_list_index },
    { "clear", m_list_clear },
};

/* dict methods */

static py_value m_dict_get(py_value self, py_value *args, int argc, uint32_t line)
{
    if (argc < 1 || argc > 2) {
        ev_setn(line, "TypeError: get() takes one or two arguments", 0, 0, 0, 0);
        return py_none();
    }
    py_value out;
    if (py_dict_get(self, args[0], &out)) {
        py_incref(out);
        return out;
    }
    if (argc == 2) {
        py_incref(args[1]);
        return args[1];
    }
    return py_none();
}

static py_value m_dict_keys(py_value self, py_value *args, int argc, uint32_t line)
{
    py_value l = py_new_list();
    py_dict *d = &self.u.obj->u.dict;
    for (uint32_t i = 0; i < d->count; i++) {
        py_value k = d->entries[i].key;
        py_incref(k);
        py_list_append(l, k);
    }
    return l;
}

static py_value m_dict_values(py_value self, py_value *args, int argc, uint32_t line)
{
    py_value l = py_new_list();
    py_dict *d = &self.u.obj->u.dict;
    for (uint32_t i = 0; i < d->count; i++) {
        py_value v = d->entries[i].val;
        py_incref(v);
        py_list_append(l, v);
    }
    return l;
}

static py_value m_dict_items(py_value self, py_value *args, int argc, uint32_t line)
{
    py_value l = py_new_list();
    py_dict *d = &self.u.obj->u.dict;
    for (uint32_t i = 0; i < d->count; i++) {
        py_value pair = py_new_list();
        py_value k = d->entries[i].key;
        py_value v = d->entries[i].val;
        py_incref(k);
        py_incref(v);
        py_list_append(pair, k);
        py_list_append(pair, v);
        py_list_append(l, pair);
    }
    return l;
}

static py_value m_dict_pop(py_value self, py_value *args, int argc, uint32_t line)
{
    if (argc < 1 || argc > 2) {
        ev_setn(line, "TypeError: pop() takes one or two arguments", 0, 0, 0, 0);
        return py_none();
    }
    py_value out;
    if (!py_dict_get(self, args[0], &out)) {
        if (argc == 2) {
            py_incref(args[1]);
            return args[1];
        }
        ev_setn(line, "KeyError: key not found", 0, 0, 0, 0);
        return py_none();
    }
    py_incref(out);
    py_dict_del(self, args[0]);
    return out;
}

static const method_entry dict_methods[] = {
    { "get", m_dict_get },
    { "keys", m_dict_keys },
    { "values", m_dict_values },
    { "items", m_dict_items },
    { "pop", m_dict_pop },
};

/* str methods */

static py_value m_str_upper(py_value self, py_value *args, int argc, uint32_t line)
{
    py_str *s = &self.u.obj->u.str;
    py_value r = py_new_strn(s->bytes, s->len);
    if (r.type == PY_STR) {
        char *p = r.u.obj->u.str.bytes;
        for (uint32_t i = 0; i < s->len; i++) {
            if (p[i] >= 'a' && p[i] <= 'z')
                p[i] = (char)(p[i] - 'a' + 'A');
        }
    }
    return r;
}

static py_value m_str_lower(py_value self, py_value *args, int argc, uint32_t line)
{
    py_str *s = &self.u.obj->u.str;
    py_value r = py_new_strn(s->bytes, s->len);
    if (r.type == PY_STR) {
        char *p = r.u.obj->u.str.bytes;
        for (uint32_t i = 0; i < s->len; i++) {
            if (p[i] >= 'A' && p[i] <= 'Z')
                p[i] = (char)(p[i] - 'A' + 'a');
        }
    }
    return r;
}

static py_value m_str_strip(py_value self, py_value *args, int argc, uint32_t line)
{
    py_str *s = &self.u.obj->u.str;
    uint32_t a = 0, b = s->len;
    while (a < b && (s->bytes[a] == ' ' || s->bytes[a] == '\t' ||
                     s->bytes[a] == '\n' || s->bytes[a] == '\r'))
        a++;
    while (b > a && (s->bytes[b - 1] == ' ' || s->bytes[b - 1] == '\t' ||
                     s->bytes[b - 1] == '\n' || s->bytes[b - 1] == '\r'))
        b--;
    return py_new_strn(s->bytes + a, b - a);
}

static py_value m_str_split(py_value self, py_value *args, int argc, uint32_t line)
{
    py_str *s = &self.u.obj->u.str;
    py_value l = py_new_list();

    if (argc == 0) {
        /* Split on runs of whitespace. */
        uint32_t i = 0;
        while (i < s->len) {
            while (i < s->len && (s->bytes[i] == ' ' || s->bytes[i] == '\t' ||
                                  s->bytes[i] == '\n' || s->bytes[i] == '\r'))
                i++;
            uint32_t start = i;
            while (i < s->len && !(s->bytes[i] == ' ' || s->bytes[i] == '\t' ||
                                   s->bytes[i] == '\n' || s->bytes[i] == '\r'))
                i++;
            if (i > start)
                py_list_append(l, py_new_strn(s->bytes + start, i - start));
        }
        return l;
    }

    if (argc != 1 || args[0].type != PY_STR) {
        ev_setn(line, "TypeError: split() takes a string separator", 0, 0, 0, 0);
        py_decref(l);
        return py_none();
    }

    py_str *sep = &args[0].u.obj->u.str;
    if (sep->len == 0) {
        ev_setn(line, "ValueError: empty separator", 0, 0, 0, 0);
        py_decref(l);
        return py_none();
    }

    uint32_t start = 0;
    for (uint32_t i = 0; i + sep->len <= s->len; ) {
        if (!memcmp(s->bytes + i, sep->bytes, sep->len)) {
            py_list_append(l, py_new_strn(s->bytes + start, i - start));
            i += sep->len;
            start = i;
            continue;
        }
        i++;
    }
    py_list_append(l, py_new_strn(s->bytes + start, s->len - start));
    return l;
}

static py_value m_str_join(py_value self, py_value *args, int argc, uint32_t line)
{
    if (argc != 1 || args[0].type != PY_LIST) {
        ev_setn(line, "TypeError: join() takes a list of strings", 0, 0, 0, 0);
        return py_none();
    }
    py_str *sep = &self.u.obj->u.str;
    py_list *l = &args[0].u.obj->u.list;

    uint32_t total = 0;
    for (uint32_t i = 0; i < l->len; i++) {
        if (l->items[i].type != PY_STR) {
            ev_setn(line, "TypeError: join() needs a list of strings", 0, 0, 0, 0);
            return py_none();
        }
        total += l->items[i].u.obj->u.str.len;
    }
    if (l->len > 1)
        total += sep->len * (l->len - 1);

    py_value r = py_new_strn(0, total);
    if (r.type != PY_STR)
        return r;
    char *w = r.u.obj->u.str.bytes;
    for (uint32_t i = 0; i < l->len; i++) {
        py_str *piece = &l->items[i].u.obj->u.str;
        if (piece->len)
            memcpy(w, piece->bytes, piece->len);
        w += piece->len;
        if (i + 1 < l->len && sep->len) {
            memcpy(w, sep->bytes, sep->len);
            w += sep->len;
        }
    }
    return r;
}

static py_value m_str_replace(py_value self, py_value *args, int argc, uint32_t line)
{
    if (argc != 2 || args[0].type != PY_STR || args[1].type != PY_STR) {
        ev_setn(line, "TypeError: replace() takes two strings", 0, 0, 0, 0);
        return py_none();
    }
    py_str *s = &self.u.obj->u.str;
    py_str *from = &args[0].u.obj->u.str;
    py_str *to = &args[1].u.obj->u.str;
    if (from->len == 0)
        return py_new_strn(s->bytes, s->len);

    uint32_t count = 0;
    for (uint32_t i = 0; i + from->len <= s->len; ) {
        if (!memcmp(s->bytes + i, from->bytes, from->len)) {
            count++;
            i += from->len;
        } else {
            i++;
        }
    }
    uint32_t total = s->len + count * to->len - count * from->len;

    py_value r = py_new_strn(0, total);
    if (r.type != PY_STR)
        return r;
    char *w = r.u.obj->u.str.bytes;
    uint32_t start = 0;
    for (uint32_t i = 0; i + from->len <= s->len; ) {
        if (!memcmp(s->bytes + i, from->bytes, from->len)) {
            uint32_t chunk = i - start;
            if (chunk)
                memcpy(w, s->bytes + start, chunk);
            w += chunk;
            if (to->len)
                memcpy(w, to->bytes, to->len);
            w += to->len;
            i += from->len;
            start = i;
            continue;
        }
        i++;
    }
    uint32_t chunk = s->len - start;
    if (chunk)
        memcpy(w, s->bytes + start, chunk);
    return r;
}

static py_value m_str_find(py_value self, py_value *args, int argc, uint32_t line)
{
    if (argc != 1 || args[0].type != PY_STR) {
        ev_setn(line, "TypeError: find() takes a string", 0, 0, 0, 0);
        return py_none();
    }
    py_str *s = &self.u.obj->u.str;
    py_str *needle = &args[0].u.obj->u.str;
    if (needle->len == 0)
        return py_int(0);
    for (uint32_t i = 0; i + needle->len <= s->len; i++) {
        if (!memcmp(s->bytes + i, needle->bytes, needle->len))
            return py_int((int32_t)i);
    }
    return py_int(-1);
}

static const method_entry str_methods[] = {
    { "upper", m_str_upper },
    { "lower", m_str_lower },
    { "strip", m_str_strip },
    { "split", m_str_split },
    { "join", m_str_join },
    { "replace", m_str_replace },
    { "find", m_str_find },
};

static const method_entry *find_method(py_value obj, const char *name)
{
    const method_entry *table = 0;
    uint32_t n = 0;

    switch (obj.type) {
    case PY_LIST: table = list_methods; n = sizeof(list_methods) / sizeof(list_methods[0]); break;
    case PY_DICT: table = dict_methods; n = sizeof(dict_methods) / sizeof(dict_methods[0]); break;
    case PY_STR:  table = str_methods;  n = sizeof(str_methods) / sizeof(str_methods[0]); break;
    default: return 0;
    }

    for (uint32_t i = 0; i < n; i++) {
        if (!strcmp(table[i].name, name))
            return &table[i];
    }
    return 0;
}

/* ---- builtins ----------------------------------------------------------- */

static py_value bi_print(struct py_call *c)
{
    for (int i = 0; i < c->argc; i++) {
        if (i)
            out_str(" ");
        py_value s = py_str_of(c->args[i]);
        if (s.type == PY_STR)
            out_bytes(s.u.obj->u.str.bytes, s.u.obj->u.str.len);
        py_decref(s);
    }
    out_str("\n");
    return py_none();
}

static py_value bi_len(struct py_call *c)
{
    if (c->argc != 1) {
        ev_setn(c->kwvals ? 0 : 0, "TypeError: len() takes exactly one argument", 0, 0, 0, 0);
        return py_none();
    }
    py_value v = c->args[0];
    if (v.type == PY_STR)
        return py_int((int32_t)v.u.obj->u.str.len);
    if (v.type == PY_LIST)
        return py_int(py_list_len(v));
    if (v.type == PY_DICT)
        return py_int(py_dict_len(v));
    ev_setn(0, "TypeError: object of type '", py_type_name(v), "' has no len()", 0, 0);
    return py_none();
}

static py_value bi_range(struct py_call *c)
{
    int32_t start = 0, stop = 0, step = 1;

    if (c->argc == 1) {
        if (!as_int(c->args[0], &stop)) {
            ev_setn(0, "TypeError: range() takes integers", 0, 0, 0, 0);
            return py_none();
        }
    } else if (c->argc == 2 || c->argc == 3) {
        if (!as_int(c->args[0], &start) || !as_int(c->args[1], &stop)) {
            ev_setn(0, "TypeError: range() takes integers", 0, 0, 0, 0);
            return py_none();
        }
        if (c->argc == 3 && !as_int(c->args[2], &step)) {
            ev_setn(0, "TypeError: range() takes integers", 0, 0, 0, 0);
            return py_none();
        }
    } else {
        ev_setn(0, "TypeError: range() takes one to three arguments", 0, 0, 0, 0);
        return py_none();
    }

    if (step == 0) {
        ev_setn(0, "ValueError: range() arg 3 must not be zero", 0, 0, 0, 0);
        return py_none();
    }

    int32_t count = 0;
    if (step > 0 && stop > start)
        count = (stop - start + step - 1) / step;
    else if (step < 0 && stop < start)
        count = (start - stop - step - 1) / (-step);

    /* A range is a list here, and a list costs memory.  Refuse the ones
     * that would not fit rather than dying in the middle of filling it. */
    if (count > 20000) {
        ev_setn(0, "MemoryError: range too large for this interpreter", 0, 0, 0, 0);
        return py_none();
    }

    py_value l = py_new_list();
    for (int32_t i = 0; i < count; i++)
        py_list_append(l, py_int(start + i * step));
    return l;
}

static py_value bi_str(struct py_call *c)
{
    if (c->argc != 1) {
        ev_setn(0, "TypeError: str() takes exactly one argument", 0, 0, 0, 0);
        return py_none();
    }
    return py_str_of(c->args[0]);
}

static py_value bi_int(struct py_call *c)
{
    if (c->argc != 1) {
        ev_setn(0, "TypeError: int() takes exactly one argument", 0, 0, 0, 0);
        return py_none();
    }
    py_value v = c->args[0];
    int32_t x;
    if (as_int(v, &x))
        return py_int(x);
    if (v.type == PY_STR) {
        py_str *s = &v.u.obj->u.str;
        uint32_t i = 0;
        int neg = 0;
        if (i < s->len && (s->bytes[i] == '-' || s->bytes[i] == '+')) {
            neg = (s->bytes[i] == '-');
            i++;
        }
        if (i >= s->len) {
            ev_setn(0, "ValueError: invalid literal for int()", 0, 0, 0, 0);
            return py_none();
        }
        int32_t acc = 0;
        for (; i < s->len; i++) {
            if (s->bytes[i] < '0' || s->bytes[i] > '9') {
                ev_setn(0, "ValueError: invalid literal for int()", 0, 0, 0, 0);
                return py_none();
            }
            acc = acc * 10 + (s->bytes[i] - '0');
        }
        return py_int(neg ? -acc : acc);
    }
    ev_setn(0, "TypeError: int() argument must be a string or a number", 0, 0, 0, 0);
    return py_none();
}

static py_value bi_type(struct py_call *c)
{
    if (c->argc != 1) {
        ev_setn(0, "TypeError: type() takes exactly one argument", 0, 0, 0, 0);
        return py_none();
    }
    return py_new_str(py_type_name(c->args[0]));
}

static py_value bi_bool(struct py_call *c)
{
    if (c->argc != 1) {
        ev_setn(0, "TypeError: bool() takes exactly one argument", 0, 0, 0, 0);
        return py_none();
    }
    return py_bool(py_truthy(c->args[0]));
}

static py_value bi_abs(struct py_call *c)
{
    int32_t x;
    if (c->argc != 1 || !as_int(c->args[0], &x)) {
        ev_setn(0, "TypeError: abs() takes exactly one integer", 0, 0, 0, 0);
        return py_none();
    }
    return py_int(x < 0 ? -x : x);
}

static py_value bi_sum(struct py_call *c)
{
    if (c->argc != 1 || c->args[0].type != PY_LIST) {
        ev_setn(0, "TypeError: sum() takes a list of numbers", 0, 0, 0, 0);
        return py_none();
    }
    py_list *l = &c->args[0].u.obj->u.list;
    int32_t acc = 0;
    for (uint32_t i = 0; i < l->len; i++) {
        int32_t x;
        if (!as_int(l->items[i], &x)) {
            ev_setn(0, "TypeError: sum() needs a list of numbers", 0, 0, 0, 0);
            return py_none();
        }
        acc += x;
    }
    return py_int(acc);
}

static py_value bi_min(struct py_call *c)
{
    if (c->argc != 1 || c->args[0].type != PY_LIST) {
        ev_setn(0, "TypeError: min() takes a list of numbers", 0, 0, 0, 0);
        return py_none();
    }
    py_list *l = &c->args[0].u.obj->u.list;
    if (!l->len) {
        ev_setn(0, "ValueError: min() of an empty list", 0, 0, 0, 0);
        return py_none();
    }
    int32_t best = 0;
    as_int(l->items[0], &best);
    for (uint32_t i = 1; i < l->len; i++) {
        int32_t x;
        if (!as_int(l->items[i], &x)) {
            ev_setn(0, "TypeError: min() needs a list of numbers", 0, 0, 0, 0);
            return py_none();
        }
        if (x < best)
            best = x;
    }
    return py_int(best);
}

static py_value bi_max(struct py_call *c)
{
    if (c->argc != 1 || c->args[0].type != PY_LIST) {
        ev_setn(0, "TypeError: max() takes a list of numbers", 0, 0, 0, 0);
        return py_none();
    }
    py_list *l = &c->args[0].u.obj->u.list;
    if (!l->len) {
        ev_setn(0, "ValueError: max() of an empty list", 0, 0, 0, 0);
        return py_none();
    }
    int32_t best = 0;
    as_int(l->items[0], &best);
    for (uint32_t i = 1; i < l->len; i++) {
        int32_t x;
        if (!as_int(l->items[i], &x)) {
            ev_setn(0, "TypeError: max() needs a list of numbers", 0, 0, 0, 0);
            return py_none();
        }
        if (x > best)
            best = x;
    }
    return py_int(best);
}

static py_value bi_list(struct py_call *c)
{
    py_value l = py_new_list();
    if (c->argc == 0)
        return l;
    if (c->argc != 1 || c->args[0].type != PY_LIST) {
        py_decref(l);
        ev_setn(0, "TypeError: list() takes a list argument", 0, 0, 0, 0);
        return py_none();
    }
    py_list *src = &c->args[0].u.obj->u.list;
    for (uint32_t i = 0; i < src->len; i++) {
        py_value it = src->items[i];
        py_incref(it);
        py_list_append(l, it);
    }
    return l;
}

static py_value bi_dict(struct py_call *c)
{
    py_value d = py_new_dict();
    if (c->argc == 0)
        return d;
    if (c->argc != 1 || c->args[0].type != PY_DICT) {
        py_decref(d);
        ev_setn(0, "TypeError: dict() takes a dict argument", 0, 0, 0, 0);
        return py_none();
    }
    py_dict *src = &c->args[0].u.obj->u.dict;
    for (uint32_t i = 0; i < src->count; i++) {
        py_value k = src->entries[i].key;
        py_value v = src->entries[i].val;
        py_incref(k);
        py_incref(v);
        py_dict_set(d, k, v);
    }
    return d;
}

typedef struct {
    const char *name;
    py_value (*fn)(struct py_call *c);
} builtin_entry;

static const builtin_entry builtins[] = {
    { "print", bi_print },
    { "len", bi_len },
    { "range", bi_range },
    { "str", bi_str },
    { "int", bi_int },
    { "type", bi_type },
    { "bool", bi_bool },
    { "abs", bi_abs },
    { "sum", bi_sum },
    { "min", bi_min },
    { "max", bi_max },
    { "list", bi_list },
    { "dict", bi_dict },
};

static py_value make_builtin(const char *name, py_value (*fn)(struct py_call *c))
{
    py_obj *o = (py_obj *)py_alloc((uint32_t)sizeof(py_obj));
    py_value v;
    v.type = PY_BUILTIN;
    if (!o) {
        v.u.obj = 0;
        return v;
    }
    o->refs = 1;
    o->type = PY_BUILTIN;
    o->u.builtin.name = name;
    o->u.builtin.fn = fn;
    v.u.obj = o;
    return v;
}

/* ---- expressions -------------------------------------------------------- */

static py_value eval_call(py_node *n, py_env *env)
{
    /* obj.m(...) - the method is looked up on the object rather than
     * evaluated as a value, because a bound method is not a thing this
     * interpreter has. */
    if (n->a->kind == PY_E_ATTR) {
        py_value obj = eval_node(n->a->a, env);
        if (ev_err)
            return py_none();
        py_value r = call_method(obj, n->a->name, n, env);
        py_decref(obj);
        return r;
    }

    py_value callee = eval_node(n->a, env);
    if (ev_err)
        return py_none();

    if (callee.type == PY_BUILTIN) {
        arglist al;
        if (collect_args(n, env, &al) != 0) {
            free_args(&al);
            py_decref(callee);
            return py_none();
        }
        struct py_call call;
        call.args = al.args;
        call.argc = al.argc;
        call.kwvals = al.kwvals;
        call.kwnames = al.kwnames;
        call.nkwargs = al.nkwargs;

        py_value r = callee.u.obj->u.builtin.fn(&call);
        free_args(&al);
        py_decref(callee);
        return r;
    }

    if (callee.type == PY_FUNC) {
        py_value r = call_function(callee.u.obj, n, env);
        py_decref(callee);
        return r;
    }

    ev_setn(n->line, "TypeError: '", py_type_name(callee), "' object is not callable", 0, 0);
    py_decref(callee);
    return py_none();
}

static py_value eval_index(py_node *n, py_env *env)
{
    py_value cont = eval_node(n->a, env);
    if (ev_err)
        return py_none();
    py_value idx = eval_node(n->b, env);
    if (ev_err) {
        py_decref(cont);
        return py_none();
    }

    if (cont.type == PY_LIST) {
        int32_t i;
        if (!as_int(idx, &i)) {
            ev_setn(n->line, "TypeError: list indices must be integers", 0, 0, 0, 0);
            py_decref(cont);
            py_decref(idx);
            return py_none();
        }
        int32_t len = py_list_len(cont);
        if (i < 0)
            i += len;
        if (i < 0 || i >= len) {
            ev_setn(n->line, "IndexError: list index out of range", 0, 0, 0, 0);
            py_decref(cont);
            py_decref(idx);
            return py_none();
        }
        py_value v = py_list_get(cont, i);
        py_incref(v);
        py_decref(cont);
        py_decref(idx);
        return v;
    }

    if (cont.type == PY_STR) {
        int32_t i;
        if (!as_int(idx, &i)) {
            ev_setn(n->line, "TypeError: string indices must be integers", 0, 0, 0, 0);
            py_decref(cont);
            py_decref(idx);
            return py_none();
        }
        py_str *s = &cont.u.obj->u.str;
        if (i < 0)
            i += (int32_t)s->len;
        if (i < 0 || i >= (int32_t)s->len) {
            ev_setn(n->line, "IndexError: string index out of range", 0, 0, 0, 0);
            py_decref(cont);
            py_decref(idx);
            return py_none();
        }
        py_value v = py_new_strn(s->bytes + i, 1);
        py_decref(cont);
        py_decref(idx);
        return v;
    }

    if (cont.type == PY_DICT) {
        py_value out;
        if (py_dict_get(cont, idx, &out)) {
            py_incref(out);
            py_decref(cont);
            py_decref(idx);
            return out;
        }
        /* A missing key is an error, the way Python reports it. */
        py_value r = py_repr(idx);
        const char *txt = "?";
        if (r.type == PY_STR)
            txt = r.u.obj->u.str.bytes;
        ev_setn(n->line, "KeyError: ", txt, 0, 0, 0);
        py_decref(r);
        py_decref(cont);
        py_decref(idx);
        return py_none();
    }

    ev_setn(n->line, "TypeError: '", py_type_name(cont), "' object is not subscriptable", 0, 0);
    py_decref(cont);
    py_decref(idx);
    return py_none();
}

static py_value eval_node(py_node *n, py_env *env)
{
    if (!n)
        return py_none();
    if (ev_oom) {
        ev_setn(n->line, "MemoryError: out of memory", 0, 0, 0, 0);
        return py_none();
    }

    switch (n->kind) {
    case PY_E_INT:
        return py_int(n->ival);

    case PY_E_STR:
        py_incref(n->u.sval);
        return n->u.sval;

    case PY_E_BOOL:
        return py_bool(n->ival);

    case PY_E_NONE:
        return py_none();

    case PY_E_NAME: {
        py_value v;
        if (!env_lookup(env, n->name, &v)) {
            ev_setn(n->line, "NameError: name '", n->name, "' is not defined", 0, 0);
            return py_none();
        }
        py_incref(v);
        return v;
    }

    case PY_E_LIST: {
        py_value l = py_new_list();
        for (py_node *e = n->a; e; e = e->next) {
            py_value v = eval_node(e, env);
            if (ev_err) {
                py_decref(l);
                return py_none();
            }
            py_list_append(l, v);
        }
        return l;
    }

    case PY_E_GROUP: {
        /* A parenthesised group is not a list: '(a)' is a, '[a]' is a
         * one-element list.  A group with several elements is the closest
         * thing to a tuple this subset has. */
        if (!n->a)
            return py_new_list();
        if (!n->a->next)
            return eval_node(n->a, env);
        py_value l = py_new_list();
        for (py_node *e = n->a; e; e = e->next) {
            py_value v = eval_node(e, env);
            if (ev_err) {
                py_decref(l);
                return py_none();
            }
            py_list_append(l, v);
        }
        return l;
    }

    case PY_E_DICT: {
        py_value d = py_new_dict();
        py_node *k = n->u.dict.keys;
        py_node *v = n->u.dict.vals;
        while (k && v) {
            py_value kv = eval_node(k, env);
            if (ev_err) {
                py_decref(d);
                return py_none();
            }
            py_value vv = eval_node(v, env);
            if (ev_err) {
                py_decref(kv);
                py_decref(d);
                return py_none();
            }
            py_dict_set(d, kv, vv);
            k = k->next;
            v = v->next;
        }
        return d;
    }

    case PY_E_BINOP: {
        /* Three shapes share this node kind: a binary operator, a
         * conditional expression (op none, ival 1) and a keyword
         * argument (op none, with a name). */
        if (n->op == PY_OP_NONE) {
            if (n->ival == 1 && n->c) {
                /* a if cond else b */
                py_value cond = eval_node(n->b, env);
                if (ev_err)
                    return py_none();
                int t = py_truthy(cond);
                py_decref(cond);
                return eval_node(t ? n->a : n->c, env);
            }
            ev_setn(n->line, "SyntaxError: cannot evaluate a keyword argument here", 0, 0, 0, 0);
            return py_none();
        }
        py_value a = eval_node(n->a, env);
        if (ev_err)
            return py_none();
        py_value b = eval_node(n->b, env);
        if (ev_err) {
            py_decref(a);
            return py_none();
        }
        py_value r = binop(n->op, a, b, n->line);
        py_decref(a);
        py_decref(b);
        return r;
    }

    case PY_E_UNARY: {
        if (n->op == PY_OP_NONE) {
            /* 'not' */
            py_value a = eval_node(n->a, env);
            if (ev_err)
                return py_none();
            int t = !py_truthy(a);
            py_decref(a);
            return py_bool(t);
        }
        py_value a = eval_node(n->a, env);
        if (ev_err)
            return py_none();
        int32_t x;
        if (!as_int(a, &x)) {
            ev_setn(n->line, "TypeError: bad operand type for unary '",
                    py_type_name(a), "'", 0, 0);
            py_decref(a);
            return py_none();
        }
        py_decref(a);
        return py_int(n->op == PY_OP_SUB ? -x : x);
    }

    case PY_E_COMPARE: {
        py_value a = eval_node(n->a, env);
        if (ev_err)
            return py_none();
        py_value b = eval_node(n->b, env);
        if (ev_err) {
            py_decref(a);
            return py_none();
        }
        py_value r;
        if (n->op == PY_OP_IN)
            r = py_bool(value_in(a, b));
        else
            r = compare_op(n->op, a, b, n->line);
        py_decref(a);
        py_decref(b);
        return r;
    }

    case PY_E_BOOLOP: {
        py_value a = eval_node(n->a, env);
        if (ev_err)
            return py_none();
        if (n->op == PY_OP_AND) {
            if (!py_truthy(a))
                return a;            /* 'a and b' is a when a is false */
            py_decref(a);
        } else {
            if (py_truthy(a))
                return a;            /* 'a or b' is a when a is true */
            py_decref(a);
        }
        return eval_node(n->b, env);
    }

    case PY_E_CALL:
        return eval_call(n, env);

    case PY_E_INDEX:
        return eval_index(n, env);

    case PY_E_ATTR:
        /* An attribute that is not called is a method reference, and this
         * interpreter has no bound methods. */
        ev_setn(n->line, "AttributeError: attributes can only be used in a "
                         "method call", 0, 0, 0, 0);
        return py_none();
    }

    return py_none();
}

/* ---- statements --------------------------------------------------------- */

static void assign_to(py_node *target, py_value v, py_env *env)
{
    if (target->kind == PY_E_NAME) {
        env_assign(env, target->name, v);
        return;
    }

    if (target->kind == PY_E_INDEX) {
        py_value cont = eval_node(target->a, env);
        if (ev_err) {
            py_decref(v);
            return;
        }
        py_value idx = eval_node(target->b, env);
        if (ev_err) {
            py_decref(cont);
            py_decref(v);
            return;
        }

        if (cont.type == PY_LIST) {
            int32_t i;
            if (!as_int(idx, &i)) {
                ev_setn(target->line, "TypeError: list indices must be integers", 0, 0, 0, 0);
            } else {
                int32_t len = py_list_len(cont);
                if (i < 0)
                    i += len;
                if (i < 0 || i >= len)
                    ev_setn(target->line, "IndexError: list assignment index out of range", 0, 0, 0, 0);
                else
                    py_list_set(cont, i, v);   /* the list takes the reference */
            }
        } else if (cont.type == PY_DICT) {
            /* The dict takes a reference to the key of its own, and this
             * scope still holds one: without the extra reference the key
             * would be released twice. */
            py_incref(idx);
            py_dict_set(cont, idx, v);          /* the dict takes both */
        } else {
            ev_setn(target->line, "TypeError: '", py_type_name(cont),
                    "' object does not support item assignment", 0, 0);
            py_decref(v);
        }

        py_decref(cont);
        py_decref(idx);
        return;
    }

    ev_setn(target->line, "SyntaxError: cannot assign to this expression", 0, 0, 0, 0);
    py_decref(v);
}

static py_flow exec_stmt(py_node *n, py_env *env)
{
    if (ev_oom) {
        ev_setn(n->line, "MemoryError: out of memory", 0, 0, 0, 0);
        return FLOW_NORMAL;
    }
    switch (n->kind) {
    case PY_S_EXPR: {
        py_value v = eval_node(n->a, env);
        py_decref(v);
        return FLOW_NORMAL;
    }

    case PY_S_ASSIGN: {
        py_value v = eval_node(n->b, env);
        if (ev_err)
            return FLOW_NORMAL;
        assign_to(n->a, v, env);
        return FLOW_NORMAL;
    }

    case PY_S_AUGASSIGN: {
        /* The target is a name; the value is in ->b and the operator in
         * ->op.  See the AST note in py_ast.h. */
        const char *name = (n->a && n->a->kind == PY_E_NAME) ? n->a->name : 0;
        if (!name) {
            ev_setn(n->line, "SyntaxError: cannot assign to this expression", 0, 0, 0, 0);
            return FLOW_NORMAL;
        }
        py_value cur;
        if (!env_lookup(env, name, &cur)) {
            ev_setn(n->line, "NameError: name '", name, "' is not defined", 0, 0);
            return FLOW_NORMAL;
        }
        py_incref(cur);
        py_value rhs = eval_node(n->b, env);
        if (ev_err) {
            py_decref(cur);
            return FLOW_NORMAL;
        }
        py_value r = binop(n->op, cur, rhs, n->line);
        py_decref(cur);
        py_decref(rhs);
        if (ev_err)
            return FLOW_NORMAL;
        env_assign(env, name, r);
        return FLOW_NORMAL;
    }

    case PY_S_IF: {
        py_value cond = eval_node(n->a, env);
        if (ev_err)
            return FLOW_NORMAL;
        int t = py_truthy(cond);
        py_decref(cond);
        if (t)
            return exec_suite(n->b, env);
        if (n->c)
            return exec_suite(n->c, env);
        return FLOW_NORMAL;
    }

    case PY_S_WHILE: {
        for (;;) {
            py_value cond = eval_node(n->a, env);
            if (ev_err)
                return FLOW_NORMAL;
            int t = py_truthy(cond);
            py_decref(cond);
            if (!t)
                break;
            py_flow fl = exec_suite(n->b, env);
            if (fl == FLOW_BREAK)
                break;
            if (fl == FLOW_RETURN)
                return fl;
            /* FLOW_CONTINUE falls through to the next test */
        }
        return FLOW_NORMAL;
    }

    case PY_S_FOR: {
        py_value it = eval_node(n->a, env);
        if (ev_err)
            return FLOW_NORMAL;
        py_value items = iterable_items(it, n->line);
        py_decref(it);
        if (ev_err)
            return FLOW_NORMAL;

        py_list *l = &items.u.obj->u.list;
        for (uint32_t i = 0; i < l->len; i++) {
            /* The scope takes over a reference, and the list still holds
             * one of its own, so the item needs a second reference.  Without
             * it the list's release would take the scope's value with it. */
            py_incref(l->items[i]);
            env_assign(env, n->name, l->items[i]);
            py_flow fl = exec_suite(n->b, env);
            if (fl == FLOW_BREAK)
                break;
            if (fl == FLOW_RETURN) {
                py_decref(items);
                return fl;
            }
        }
        py_decref(items);
        return FLOW_NORMAL;
    }

    case PY_S_FUNC: {
        py_obj *o = (py_obj *)py_alloc((uint32_t)sizeof(py_obj));
        if (!o) {
            ev_setn(n->line, "MemoryError: out of memory", 0, 0, 0, 0);
            return FLOW_NORMAL;
        }
        o->refs = 1;
        o->type = PY_FUNC;
        o->u.func.name = n->name;
        o->u.func.argc = (int)n->u.fn.nparams;
        o->u.func.body = n->u.fn.body;
        o->u.func.closure = env;
        o->u.func.def = n;

        py_value f;
        f.type = PY_FUNC;
        f.u.obj = o;
        env_define(env, n->name, f);
        return FLOW_NORMAL;
    }

    case PY_S_RETURN: {
        py_value v = n->a ? eval_node(n->a, env) : py_none();
        if (ev_err)
            return FLOW_NORMAL;
        py_decref(env->ret);
        env->ret = v;
        return FLOW_RETURN;
    }

    case PY_S_DEL: {
        py_node *t = n->a;
        if (!t) {
            ev_setn(n->line, "SyntaxError: nothing to delete", 0, 0, 0, 0);
            return FLOW_NORMAL;
        }

        if (t->kind == PY_E_NAME) {
            if (!env_del(env, t->name)) {
                ev_setn(n->line, "NameError: name '", t->name, "' is not defined", 0, 0);
            }
            return FLOW_NORMAL;
        }

        if (t->kind == PY_E_INDEX) {
            /* The target is an index node: a is the container, b the key. */
            py_value cont = eval_node(t->a, env);
            if (ev_err)
                return FLOW_NORMAL;
            py_value idx = eval_node(t->b, env);
            if (ev_err) {
                py_decref(cont);
                return FLOW_NORMAL;
            }

            if (cont.type == PY_DICT) {
                if (!py_dict_del(cont, idx))
                    ev_setn(n->line, "KeyError: key not found", 0, 0, 0, 0);
            } else if (cont.type == PY_LIST) {
                int32_t i;
                if (!as_int(idx, &i)) {
                    ev_setn(n->line, "TypeError: list indices must be integers", 0, 0, 0, 0);
                } else {
                    int32_t len = py_list_len(cont);
                    int32_t j = (i < 0) ? i + len : i;
                    if (j < 0 || j >= len)
                        ev_setn(n->line, "IndexError: list assignment index out of range", 0, 0, 0, 0);
                    else {
                        /* py_list_pop hands the element's reference over,
                         * so releasing it is ours to do. */
                        py_value v = py_list_pop(cont, j);
                        py_decref(v);
                    }
                }
            } else {
                ev_setn(n->line, "TypeError: '", py_type_name(cont),
                        "' object does not support item deletion", 0, 0);
            }

            py_decref(cont);
            py_decref(idx);
            return FLOW_NORMAL;
        }

        ev_setn(n->line, "SyntaxError: cannot delete this expression", 0, 0, 0, 0);
        return FLOW_NORMAL;
    }

    case PY_S_BREAK:
        return FLOW_BREAK;

    case PY_S_CONTINUE:
        return FLOW_CONTINUE;

    case PY_S_PASS:
        return FLOW_NORMAL;

    case PY_S_BLOCK:
        return exec_block(n->list, env);
    }

    return FLOW_NORMAL;
}

static py_flow exec_block(py_node *first, py_env *env)
{
    for (py_node *st = first; st; st = st->next) {
        py_flow fl = exec_stmt(st, env);
        if (fl != FLOW_NORMAL)
            return fl;
        if (ev_err)
            return FLOW_NORMAL;
    }
    return FLOW_NORMAL;
}

/* A suite is either a block node or a single statement on the same line
 * as the colon. */
static py_flow exec_suite(py_node *suite, py_env *env)
{
    if (!suite)
        return FLOW_NORMAL;
    if (suite->kind == PY_S_BLOCK)
        return exec_block(suite->list, env);
    return exec_stmt(suite, env);
}

/* ---- running a module ---------------------------------------------------- */

int py_run(py_scope *mod)
{
    ev_err = 0;
    ev_line = 0;

    if (!mod) {
        ev_setn(0, "SyntaxError: nothing to run", 0, 0, 0, 0);
        return -1;
    }

    py_env *env = env_new(0);
    if (!env) {
        ev_setn(0, "MemoryError: out of memory", 0, 0, 0, 0);
        return -1;
    }

    /* The builtins are ordinary names in the module scope. */
    for (uint32_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        env_define(env, builtins[i].name, make_builtin(builtins[i].name, builtins[i].fn));

    exec_suite(mod->body, env);
    if (ev_err)
        return -1;
    return 0;
}
