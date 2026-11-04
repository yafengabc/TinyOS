/* py_rt.c - the runtime the bytecode runs on.
 *
 * The bytecode does not know what a list is.  It knows that names are
 * found in scopes, that `+` adds, that `print` prints.  All of that is
 * here.
 *
 * The scopes are the interesting part.  They are dynamic: a name is
 * assigned where it is already bound, searching outward, and defined in
 * the current scope otherwise.  That is what makes the counter idiom
 * work without a `global` statement - `checks = checks + 1` inside a
 * function updates the `checks` of the scope outside, because that is
 * where the name is bound.
 *
 * It also means the scope cannot be a fixed set of slots decided at
 * compile time, which is why the bytecode carries names and not slot
 * numbers.  A compiler that resolved a name to a slot would change
 * what the language means.
 */

#include "py_rt.h"
#include "py_mem.h"
#include "py_str.h"
#include "lib.h"

/* ---- errors ------------------------------------------------------------ */

/* The error message is built in one buffer, so a name or a type can be
 * woven into it.  The first error wins: a failed expression leaves the
 * rest of the program unevaluated, and the one that failed is the one
 * worth reporting. */
#define RT_MAX 200
static char rt_buf[RT_MAX];
static const char *rt_err;
static uint32_t rt_line;

static char *app(char *w, char *end, const char *s)
{
    while (*s && w < end - 1)
        *w++ = *s++;
    return w;
}

/* Build an error message out of up to five pieces.  A name or a type is
 * woven into it, so the pieces are joined rather than concatenated. */
void rt_error5(uint32_t line, const char *a, const char *b, const char *c,
                    const char *d, const char *e)
{
    if (rt_err)
        return;
    char *w = rt_buf;
    char *end = rt_buf + RT_MAX;
    w = app(w, end, a);
    w = app(w, end, b);
    w = app(w, end, c);
    w = app(w, end, d);
    w = app(w, end, e);
    *w = 0;
    rt_err = rt_buf;
    rt_line = line;
}

void py_rt_error(uint32_t line, const char *msg)
{
    if (rt_err)
        return;
    rt_error5(line, msg, 0, 0, 0, 0);
}

const char *py_rt_error_msg(void) { return rt_err; }
uint32_t py_rt_error_line(void) { return rt_line; }

void py_rt_error_clear(void)
{
    rt_err = 0;
    rt_line = 0;
}

/* ---- scopes -------------------------------------------------------------- */

/* A scope is a hash table of names.  The names are the program's name
 * table entries, so two scopes that hold the same name hold the same
 * pointer and the comparison is a pointer comparison.  The table's
 * capacity is a power of two, so the index is the hash masked off the
 * top. */
typedef struct py_binding {
    const char *name;
    py_value val;
    uint8_t dead;        /* deleted by `del`: the slot stays, the name does not */
} py_binding;

struct py_scope {
    py_binding *slots;
    uint32_t cap;
    uint32_t count;
    py_scope *parent;
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

py_scope *py_scope_new(py_scope *parent)
{
    py_scope *s = (py_scope *)py_alloc((uint32_t)sizeof(py_scope));
    if (!s)
        return 0;
    s->slots = 0;
    s->cap = 0;
    s->count = 0;
    s->parent = parent;
    return s;
}

static int scope_grow(py_scope *s)
{
    /* Two slots to start.  Every call opens a scope, and a function
     * with one parameter is the common case; the table doubles when it
     * needs to, so this costs a rehash later and saves a block now. */
    uint32_t ncap = s->cap ? s->cap * 2 : 2;
    py_binding *ns = (py_binding *)py_alloc(ncap * (uint32_t)sizeof(py_binding));
    if (!ns)
        return -1;
    for (uint32_t i = 0; i < ncap; i++) {
        ns[i].name = 0;
        ns[i].val = py_none();
        ns[i].dead = 0;
    }
    for (uint32_t i = 0; i < s->cap; i++) {
        if (!s->slots[i].name)
            continue;
        uint32_t j = hash_name(s->slots[i].name) & (ncap - 1);
        while (ns[j].name)
            j = (j + 1) & (ncap - 1);
        ns[j] = s->slots[i];
    }
    if (s->slots)
        py_free_auto(s->slots);
    s->slots = ns;
    s->cap = ncap;
    return 0;
}

/* The slot that holds this name, whether the name is live or was
 * deleted.  A deleted name keeps its slot so the probe sequence stays
 * intact, and so that assigning to it again revives it rather than
 * making a second one. */
static py_binding *scope_find_slot(py_scope *s, const char *name)
{
    if (!s->cap)
        return 0;
    uint32_t mask = s->cap - 1;
    uint32_t i = hash_name(name) & mask;
    while (s->slots[i].name) {
        if (s->slots[i].name == name)
            return &s->slots[i];
        i = (i + 1) & mask;
    }
    return 0;
}

void py_scope_define(py_scope *s, const char *name, py_value v)
{
    py_binding *b = scope_find_slot(s, name);
    if (b) {
        py_decref(b->val);
        b->val = v;
        if (b->dead) {
            b->dead = 0;
            s->count++;
        }
        return;
    }
    if ((s->count + 1) * 4 >= s->cap * 3) {
        if (scope_grow(s) != 0) {
            py_decref(v);
            return;
        }
    }
    uint32_t mask = s->cap - 1;
    uint32_t i = hash_name(name) & mask;
    while (s->slots[i].name)
        i = (i + 1) & mask;
    s->slots[i].name = name;
    s->slots[i].val = v;
    s->count++;
}

void py_scope_assign(py_scope *s, const char *name, py_value v)
{
    for (py_scope *p = s; p; p = p->parent) {
        py_binding *b = scope_find_slot(p, name);
        if (b) {
            py_decref(b->val);
            b->val = v;
            if (b->dead) {
                b->dead = 0;
                p->count++;
            }
            return;
        }
    }
    py_scope_define(s, name, v);
}

int py_scope_lookup(py_scope *s, const char *name, py_value *out)
{
    for (py_scope *p = s; p; p = p->parent) {
        py_binding *b = scope_find_slot(p, name);
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

int py_scope_delete(py_scope *s, const char *name)
{
    py_binding *b = scope_find_slot(s, name);
    if (!b || b->dead)
        return 0;
    py_decref(b->val);
    b->val = py_none();
    b->dead = 1;
    s->count--;
    return 1;
}

int py_scope_bound(py_scope *s, const char *name)
{
    py_binding *b = scope_find_slot(s, name);
    return b && !b->dead;
}

/* Release a scope: the values it holds go back to the allocator, and so
 * does the table.  A scope that other scopes point at - a closure's
 * parent - is not released here; the frame that owns it does that when
 * it goes. */
void py_scope_release(py_scope *s)
{
    if (!s)
        return;
    for (uint32_t i = 0; i < s->cap; i++) {
        if (s->slots[i].name)
            py_decref(s->slots[i].val);
    }
    if (s->slots)
        py_free_auto(s->slots);
    py_free_auto(s);
}

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

/* The binary operators, in the order the bytecode's opcodes use them.
 * The compiler and the VM agree on this numbering; it is the one place
 * where the two halves of the interpreter have to match exactly. */
enum {
    PY_OP_ADD = 0, PY_OP_SUB, PY_OP_MUL, PY_OP_DIV, PY_OP_MOD
};

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
                rt_error5(line, "ZeroDivisionError: division by zero", 0, 0, 0, 0);
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
                rt_error5(line, "ZeroDivisionError: modulo by zero", 0, 0, 0, 0);
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

    rt_error5(line, "TypeError: unsupported operand type(s) for ", 0, 0, 0, 0);
    return py_none();
}

int py_op_in(py_value item, py_value container, uint32_t line)
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

int py_op_eq(py_value a, py_value b) { return py_eq(a, b); }
int py_op_ne(py_value a, py_value b) { return !py_eq(a, b); }

/* The comparisons.  They take the two operands and say whether the
 * first is less than the second, and so on; a string compares by
 * content and anything else is an error. */
int py_op_lt(py_value a, py_value b, uint32_t line)
{

    int32_t x = 0, y = 0;
    if (as_int(a, &x) && as_int(b, &y)) {
        return x < y;
    }
    if (a.type == PY_STR && b.type == PY_STR) {
        py_str *sa = &a.u.obj->u.str;
        py_str *sb = &b.u.obj->u.str;
        uint32_t n = sa->len < sb->len ? sa->len : sb->len;
        int c = n ? memcmp(sa->bytes, sb->bytes, n) : 0;
        int cmp = c ? c : (sa->len < sb->len ? -1 : (sa->len > sb->len ? 1 : 0));
        return cmp < 0;
    }
    rt_error5(line, "TypeError: '", py_type_name(a), "' not supported between "
                  "instances of '", py_type_name(b), "'");
    return 0;
}

/* Unary minus. */
int py_op_le(py_value a, py_value b, uint32_t line)
{
    int32_t x = 0, y = 0;
    if (as_int(a, &x) && as_int(b, &y))
        return x <= y;
    if (a.type == PY_STR && b.type == PY_STR) {
        py_str *sa = &a.u.obj->u.str;
        py_str *sb = &b.u.obj->u.str;
        uint32_t n = sa->len < sb->len ? sa->len : sb->len;
        int c = n ? memcmp(sa->bytes, sb->bytes, n) : 0;
        int cmp = c ? c : (sa->len < sb->len ? -1 : (sa->len > sb->len ? 1 : 0));
        return cmp <= 0;
    }
    rt_error5(line, "TypeError: '", py_type_name(a), "' not supported between "
              "instances of '", py_type_name(b), "'");
    return 0;
}

int py_op_gt(py_value a, py_value b, uint32_t line)
{
    int32_t x = 0, y = 0;
    if (as_int(a, &x) && as_int(b, &y))
        return x > y;
    if (a.type == PY_STR && b.type == PY_STR) {
        py_str *sa = &a.u.obj->u.str;
        py_str *sb = &b.u.obj->u.str;
        uint32_t n = sa->len < sb->len ? sa->len : sb->len;
        int c = n ? memcmp(sa->bytes, sb->bytes, n) : 0;
        int cmp = c ? c : (sa->len < sb->len ? -1 : (sa->len > sb->len ? 1 : 0));
        return cmp > 0;
    }
    rt_error5(line, "TypeError: '", py_type_name(a), "' not supported between "
              "instances of '", py_type_name(b), "'");
    return 0;
}

int py_op_ge(py_value a, py_value b, uint32_t line)
{
    int32_t x = 0, y = 0;
    if (as_int(a, &x) && as_int(b, &y))
        return x >= y;
    if (a.type == PY_STR && b.type == PY_STR) {
        py_str *sa = &a.u.obj->u.str;
        py_str *sb = &b.u.obj->u.str;
        uint32_t n = sa->len < sb->len ? sa->len : sb->len;
        int c = n ? memcmp(sa->bytes, sb->bytes, n) : 0;
        int cmp = c ? c : (sa->len < sb->len ? -1 : (sa->len > sb->len ? 1 : 0));
        return cmp >= 0;
    }
    rt_error5(line, "TypeError: '", py_type_name(a), "' not supported between "
              "instances of '", py_type_name(b), "'");
    return 0;
}

py_value py_op_neg(py_value a, uint32_t line)
{
    int32_t x = 0;
    if (a.type == PY_BOOL)
        x = a.u.i ? 1 : 0;
    else if (a.type == PY_INT)
        x = a.u.i;
    else {
        rt_error5(line, "TypeError: bad operand type for unary '-': '",
                  py_type_name(a), "'", 0, 0);
        return py_none();
    }
    return py_int(-x);
}

/* The operators, as the VM calls them. */
py_value py_op_add(py_value a, py_value b, uint32_t line) { return binop(PY_OP_ADD, a, b, line); }
py_value py_op_sub(py_value a, py_value b, uint32_t line) { return binop(PY_OP_SUB, a, b, line); }
py_value py_op_mul(py_value a, py_value b, uint32_t line) { return binop(PY_OP_MUL, a, b, line); }
py_value py_op_div(py_value a, py_value b, uint32_t line) { return binop(PY_OP_DIV, a, b, line); }
py_value py_op_mod(py_value a, py_value b, uint32_t line) { return binop(PY_OP_MOD, a, b, line); }

/* ---- containers as iterables -------------------------------------------- */

/* The items an iterable yields, as a fresh list.  Copying the list first
 * means a loop body that appends to the list it is iterating does not
 * change the loop underfoot. */
py_value py_iterable_items(py_value v, uint32_t line)
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

    rt_error5(line, "TypeError: '", py_type_name(v), "' is not iterable", 0, 0);
    py_decref(l);
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



static py_value m_list_append(py_value self, py_value *args, int argc, uint32_t line)
{
    if (argc != 1) {
        rt_error5(line, "TypeError: append() takes exactly one argument", 0, 0, 0, 0);
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
        rt_error5(line, "TypeError: pop() takes at most one argument", 0, 0, 0, 0);
        return py_none();
    }
    if (argc == 1 && !as_int(args[0], &idx)) {
        rt_error5(line, "TypeError: pop() index must be an int", 0, 0, 0, 0);
        return py_none();
    }
    int32_t len = py_list_len(self);
    int32_t i = idx;
    if (i < 0)
        i += len;
    if (i < 0 || i >= len) {
        rt_error5(line, "IndexError: pop index out of range", 0, 0, 0, 0);
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
        rt_error5(line, "TypeError: insert() takes exactly two arguments", 0, 0, 0, 0);
        return py_none();
    }
    int32_t idx = 0;
    if (!as_int(args[0], &idx)) {
        rt_error5(line, "TypeError: insert() index must be an int", 0, 0, 0, 0);
        return py_none();
    }
    py_incref(args[1]);
    py_list_insert(self, idx, args[1]);
    return py_none();
}

static py_value m_list_remove(py_value self, py_value *args, int argc, uint32_t line)
{
    if (argc != 1) {
        rt_error5(line, "TypeError: remove() takes exactly one argument", 0, 0, 0, 0);
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
    rt_error5(line, "ValueError: list.remove(x): x not in list", 0, 0, 0, 0);
    return py_none();
}

static py_value m_list_index(py_value self, py_value *args, int argc, uint32_t line)
{
    if (argc != 1) {
        rt_error5(line, "TypeError: index() takes exactly one argument", 0, 0, 0, 0);
        return py_none();
    }
    py_list *l = &self.u.obj->u.list;
    for (uint32_t i = 0; i < l->len; i++) {
        if (py_eq(l->items[i], args[0]))
            return py_int((int32_t)i);
    }
    rt_error5(line, "ValueError: value is not in list", 0, 0, 0, 0);
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
        rt_error5(line, "TypeError: get() takes one or two arguments", 0, 0, 0, 0);
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
        rt_error5(line, "TypeError: pop() takes one or two arguments", 0, 0, 0, 0);
        return py_none();
    }
    py_value out;
    if (!py_dict_get(self, args[0], &out)) {
        if (argc == 2) {
            py_incref(args[1]);
            return args[1];
        }
        rt_error5(line, "KeyError: key not found", 0, 0, 0, 0);
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
        rt_error5(line, "TypeError: split() takes a string separator", 0, 0, 0, 0);
        py_decref(l);
        return py_none();
    }

    py_str *sep = &args[0].u.obj->u.str;
    if (sep->len == 0) {
        rt_error5(line, "ValueError: empty separator", 0, 0, 0, 0);
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
        rt_error5(line, "TypeError: join() takes a list of strings", 0, 0, 0, 0);
        return py_none();
    }
    py_str *sep = &self.u.obj->u.str;
    py_list *l = &args[0].u.obj->u.list;

    uint32_t total = 0;
    for (uint32_t i = 0; i < l->len; i++) {
        if (l->items[i].type != PY_STR) {
            rt_error5(line, "TypeError: join() needs a list of strings", 0, 0, 0, 0);
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
        rt_error5(line, "TypeError: replace() takes two strings", 0, 0, 0, 0);
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
        rt_error5(line, "TypeError: find() takes a string", 0, 0, 0, 0);
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

/* Call a method on an object.  Returns 0 when the object has no such
 * method, and leaves the message in the error slot. */
int py_call_method(py_value obj, const char *name, py_value *args, int argc,
                   uint32_t line, py_value *out)
{
    const method_entry *m = find_method(obj, name);
    if (!m) {
        rt_error5(line, "AttributeError: '", py_type_name(obj),
                "' object has no attribute '", name, "'");
        return 0;
    }
    *out = m->fn(obj, args, argc, line);
    return 1;
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
        rt_error5(c->kwvals ? 0 : 0, "TypeError: len() takes exactly one argument", 0, 0, 0, 0);
        return py_none();
    }
    py_value v = c->args[0];
    if (v.type == PY_STR)
        return py_int((int32_t)v.u.obj->u.str.len);
    if (v.type == PY_LIST)
        return py_int(py_list_len(v));
    if (v.type == PY_DICT)
        return py_int(py_dict_len(v));
    rt_error5(0, "TypeError: object of type '", py_type_name(v), "' has no len()", 0, 0);
    return py_none();
}

static py_value bi_range(struct py_call *c)
{
    int32_t start = 0, stop = 0, step = 1;

    if (c->argc == 1) {
        if (!as_int(c->args[0], &stop)) {
            rt_error5(0, "TypeError: range() takes integers", 0, 0, 0, 0);
            return py_none();
        }
    } else if (c->argc == 2 || c->argc == 3) {
        if (!as_int(c->args[0], &start) || !as_int(c->args[1], &stop)) {
            rt_error5(0, "TypeError: range() takes integers", 0, 0, 0, 0);
            return py_none();
        }
        if (c->argc == 3 && !as_int(c->args[2], &step)) {
            rt_error5(0, "TypeError: range() takes integers", 0, 0, 0, 0);
            return py_none();
        }
    } else {
        rt_error5(0, "TypeError: range() takes one to three arguments", 0, 0, 0, 0);
        return py_none();
    }

    if (step == 0) {
        rt_error5(0, "ValueError: range() arg 3 must not be zero", 0, 0, 0, 0);
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
        rt_error5(0, "MemoryError: range too large for this interpreter", 0, 0, 0, 0);
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
        rt_error5(0, "TypeError: str() takes exactly one argument", 0, 0, 0, 0);
        return py_none();
    }
    return py_str_of(c->args[0]);
}

static py_value bi_int(struct py_call *c)
{
    if (c->argc != 1) {
        rt_error5(0, "TypeError: int() takes exactly one argument", 0, 0, 0, 0);
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
            rt_error5(0, "ValueError: invalid literal for int()", 0, 0, 0, 0);
            return py_none();
        }
        int32_t acc = 0;
        for (; i < s->len; i++) {
            if (s->bytes[i] < '0' || s->bytes[i] > '9') {
                rt_error5(0, "ValueError: invalid literal for int()", 0, 0, 0, 0);
                return py_none();
            }
            acc = acc * 10 + (s->bytes[i] - '0');
        }
        return py_int(neg ? -acc : acc);
    }
    rt_error5(0, "TypeError: int() argument must be a string or a number", 0, 0, 0, 0);
    return py_none();
}

static py_value bi_type(struct py_call *c)
{
    if (c->argc != 1) {
        rt_error5(0, "TypeError: type() takes exactly one argument", 0, 0, 0, 0);
        return py_none();
    }
    return py_new_str(py_type_name(c->args[0]));
}

static py_value bi_bool(struct py_call *c)
{
    if (c->argc != 1) {
        rt_error5(0, "TypeError: bool() takes exactly one argument", 0, 0, 0, 0);
        return py_none();
    }
    return py_bool(py_truthy(c->args[0]));
}

static py_value bi_abs(struct py_call *c)
{
    int32_t x;
    if (c->argc != 1 || !as_int(c->args[0], &x)) {
        rt_error5(0, "TypeError: abs() takes exactly one integer", 0, 0, 0, 0);
        return py_none();
    }
    return py_int(x < 0 ? -x : x);
}

static py_value bi_sum(struct py_call *c)
{
    if (c->argc != 1 || c->args[0].type != PY_LIST) {
        rt_error5(0, "TypeError: sum() takes a list of numbers", 0, 0, 0, 0);
        return py_none();
    }
    py_list *l = &c->args[0].u.obj->u.list;
    int32_t acc = 0;
    for (uint32_t i = 0; i < l->len; i++) {
        int32_t x;
        if (!as_int(l->items[i], &x)) {
            rt_error5(0, "TypeError: sum() needs a list of numbers", 0, 0, 0, 0);
            return py_none();
        }
        acc += x;
    }
    return py_int(acc);
}

static py_value bi_min(struct py_call *c)
{
    if (c->argc != 1 || c->args[0].type != PY_LIST) {
        rt_error5(0, "TypeError: min() takes a list of numbers", 0, 0, 0, 0);
        return py_none();
    }
    py_list *l = &c->args[0].u.obj->u.list;
    if (!l->len) {
        rt_error5(0, "ValueError: min() of an empty list", 0, 0, 0, 0);
        return py_none();
    }
    int32_t best = 0;
    as_int(l->items[0], &best);
    for (uint32_t i = 1; i < l->len; i++) {
        int32_t x;
        if (!as_int(l->items[i], &x)) {
            rt_error5(0, "TypeError: min() needs a list of numbers", 0, 0, 0, 0);
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
        rt_error5(0, "TypeError: max() takes a list of numbers", 0, 0, 0, 0);
        return py_none();
    }
    py_list *l = &c->args[0].u.obj->u.list;
    if (!l->len) {
        rt_error5(0, "ValueError: max() of an empty list", 0, 0, 0, 0);
        return py_none();
    }
    int32_t best = 0;
    as_int(l->items[0], &best);
    for (uint32_t i = 1; i < l->len; i++) {
        int32_t x;
        if (!as_int(l->items[i], &x)) {
            rt_error5(0, "TypeError: max() needs a list of numbers", 0, 0, 0, 0);
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
        rt_error5(0, "TypeError: list() takes a list argument", 0, 0, 0, 0);
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
        rt_error5(0, "TypeError: dict() takes a dict argument", 0, 0, 0, 0);
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

const py_builtin py_builtins[] = {
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


/* ---- the builtins in the module scope ------------------------------------ */

/* The builtins are ordinary names in the module scope, exactly as they
 * were when the evaluator walked the tree, so a script can shadow
 * `print` with its own. */
uint32_t py_builtins_count(void)
{
    return (uint32_t)(sizeof(py_builtins) / sizeof(py_builtins[0]));
}

void py_scope_define_builtins(py_scope *s, const char **names)
{
    uint32_t n = py_builtins_count();
    for (uint32_t i = 0; i < n; i++)
        py_scope_define(s, names[i], make_builtin(names[i], py_builtins[i].fn));
}
