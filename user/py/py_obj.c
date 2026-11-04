/* py_obj.c - values, containers and reference counting. */

#include "py_obj.h"
#include "py_str.h"

/* ---- constructors ---------------------------------------------------- */

py_value py_int(int32_t v)
{
    py_value r;
    r.type = PY_INT;
    r.u.i = v;
    return r;
}

py_value py_bool(int v)
{
    py_value r;
    r.type = PY_BOOL;
    r.u.i = v ? 1 : 0;
    return r;
}

py_value py_none(void)
{
    py_value r;
    r.type = PY_NONE;
    r.u.i = 0;
    return r;
}

py_value py_new_strn(const char *s, uint32_t len)
{
    py_obj *o = (py_obj *)py_alloc((uint32_t)sizeof(py_obj) + len + 1);
    if (!o)
        return py_none();
    o->refs = 1;
    o->type = PY_STR;
    o->u.str.len = len;
    o->u.str.bytes = (char *)(o + 1);
    if (len && s)
        memcpy(o->u.str.bytes, s, len);
    o->u.str.bytes[len] = 0;

    py_value r;
    r.type = PY_STR;
    r.u.obj = o;
    return r;
}

py_value py_new_str(const char *s)
{
    return py_new_strn(s, s ? (uint32_t)strlen(s) : 0);
}

py_value py_new_list(void)
{
    py_obj *o = (py_obj *)py_alloc((uint32_t)sizeof(py_obj));
    if (!o)
        return py_none();
    o->refs = 1;
    o->type = PY_LIST;
    o->u.list.len = 0;
    o->u.list.cap = 0;
    o->u.list.items = 0;

    py_value r;
    r.type = PY_LIST;
    r.u.obj = o;
    return r;
}

py_value py_new_dict(void)
{
    py_obj *o = (py_obj *)py_alloc((uint32_t)sizeof(py_obj));
    if (!o)
        return py_none();
    o->refs = 1;
    o->type = PY_DICT;
    o->u.dict.count = 0;
    o->u.dict.cap = 0;
    o->u.dict.entries = 0;

    py_value r;
    r.type = PY_DICT;
    r.u.obj = o;
    return r;
}

/* ---- reference counting ----------------------------------------------- */

void py_incref(py_value v)
{
    if (v.type >= PY_STR && v.u.obj)
        v.u.obj->refs++;
}

void py_decref(py_value v)
{
    if (v.type < PY_STR || !v.u.obj)
        return;
    py_obj *o = v.u.obj;
    if (--o->refs > 0)
        return;

    /* Last reference: release what this object holds, then the object. */
    if (o->type == PY_LIST) {
        for (uint32_t i = 0; i < o->u.list.len; i++)
            py_decref(o->u.list.items[i]);
        if (o->u.list.items)
            py_free_auto(o->u.list.items);
    } else if (o->type == PY_DICT) {
        for (uint32_t i = 0; i < o->u.dict.count; i++) {
            py_decref(o->u.dict.entries[i].key);
            py_decref(o->u.dict.entries[i].val);
        }
        if (o->u.dict.entries)
            py_free_auto(o->u.dict.entries);
    } else if (o->type == PY_STR) {
        /* The bytes live inside the object's own block (py_new_strn() packs the
         * header and the payload into one allocation), so there is nothing
         * to release separately - freeing the object frees the bytes. */
    }
    py_free_auto(o);
}

/* ---- predicates ------------------------------------------------------- */

int py_truthy(py_value v)
{
    switch (v.type) {
    case PY_NONE:
        return 0;
    case PY_INT:
        return v.u.i != 0;
    case PY_BOOL:
        return v.u.i != 0;
    case PY_STR:
        return v.u.obj->u.str.len != 0;
    case PY_LIST:
        return v.u.obj->u.list.len != 0;
    case PY_DICT:
        return v.u.obj->u.dict.count != 0;
    default:
        return 1;
    }
}

const char *py_type_name(py_value v)
{
    switch (v.type) {
    case PY_NONE:  return "NoneType";
    case PY_INT:   return "int";
    case PY_BOOL:  return "bool";
    case PY_STR:   return "str";
    case PY_LIST:  return "list";
    case PY_DICT:  return "dict";
    case PY_FUNC:  return "function";
    case PY_BUILTIN: return "builtin_function";
    }
    return "object";
}

int py_eq(py_value a, py_value b)
{
    /* bool is a subclass of int in Python: True == 1. */
    if (a.type == PY_INT && b.type == PY_BOOL)
        return a.u.i == (b.u.i ? 1 : 0);
    if (a.type == PY_BOOL && b.type == PY_INT)
        return (a.u.i ? 1 : 0) == b.u.i;
    if (a.type != b.type)
        return 0;

    switch (a.type) {
    case PY_NONE:
        return 1;
    case PY_INT:
    case PY_BOOL:
        return a.u.i == b.u.i;
    case PY_STR: {
        py_str *x = &a.u.obj->u.str;
        py_str *y = &b.u.obj->u.str;
        if (x->len != y->len)
            return 0;
        return memcmp(x->bytes, y->bytes, x->len) == 0;
    }
    case PY_LIST: {
        /* Two lists are equal when they hold equal things in the same
         * order, the way Python compares them. */
        py_list *x = &a.u.obj->u.list;
        py_list *y = &b.u.obj->u.list;
        if (x->len != y->len)
            return 0;
        for (uint32_t i = 0; i < x->len; i++) {
            if (!py_eq(x->items[i], y->items[i]))
                return 0;
        }
        return 1;
    }
    case PY_DICT: {
        /* Two dicts are equal when they have the same keys and the same
         * values.  The order does not matter - that is the one place
         * where Python's order is not part of the value. */
        py_dict *x = &a.u.obj->u.dict;
        py_dict *y = &b.u.obj->u.dict;
        if (x->count != y->count)
            return 0;
        for (uint32_t i = 0; i < x->count; i++) {
            py_value other;
            if (!py_dict_get(b, x->entries[i].key, &other))
                return 0;
            if (!py_eq(x->entries[i].val, other))
                return 0;
        }
        return 1;
    }
    default:
        return a.u.obj == b.u.obj;
    }
}

/* FNV-1a, the hash Python uses for strings. */
uint32_t py_hash(py_value v)
{
    if (v.type == PY_INT || v.type == PY_BOOL) {
        uint32_t x = (uint32_t)v.u.i;
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
    }
    if (v.type == PY_STR) {
        py_str *s = &v.u.obj->u.str;
        uint32_t h = 2166136261u;
        for (uint32_t i = 0; i < s->len; i++) {
            h ^= (unsigned char)s->bytes[i];
            h *= 16777619u;
        }
        return h;
    }
    return 0;
}

/* ---- lists ------------------------------------------------------------ */

int32_t py_list_len(py_value list)
{
    if (list.type != PY_LIST)
        return 0;
    return (int32_t)list.u.obj->u.list.len;
}

/* Grow the backing array.  The array is reallocated rather than grown in
 * place: the interpreter's blocks come from size classes, so a bigger
 * array is a different class and cannot be extended where it lies.  The
 * old array is freed, so the memory comes back to the class it came from. */
static int list_reserve(py_obj *o, uint32_t want)
{
    if (want <= o->u.list.cap)
        return 0;
    uint32_t cap = o->u.list.cap ? o->u.list.cap * 2 : 8;
    while (cap < want)
        cap *= 2;
    py_value *items = (py_value *)py_alloc(cap * (uint32_t)sizeof(py_value));
    if (!items)
        return -1;
    if (o->u.list.items) {
        memcpy(items, o->u.list.items, o->u.list.len * sizeof(py_value));
        py_free_auto(o->u.list.items);
    }
    o->u.list.items = items;
    o->u.list.cap = cap;
    return 0;
}

void py_list_append(py_value list, py_value item)
{
    if (list.type != PY_LIST)
        return;
    py_obj *o = list.u.obj;
    if (list_reserve(o, o->u.list.len + 1) != 0)
        return;
    /* The list takes over the reference: the caller's copy is gone. */
    o->u.list.items[o->u.list.len++] = item;
}

void py_list_insert(py_value list, int32_t index, py_value v)
{
    if (list.type != PY_LIST)
        return;
    py_obj *o = list.u.obj;
    if (index < 0)
        index += (int32_t)o->u.list.len;
    if (index < 0)
        index = 0;
    if (index > (int32_t)o->u.list.len)
        index = (int32_t)o->u.list.len;
    if (list_reserve(o, o->u.list.len + 1) != 0)
        return;
    for (uint32_t i = o->u.list.len; i > (uint32_t)index; i--)
        o->u.list.items[i] = o->u.list.items[i - 1];
    o->u.list.items[index] = v;
    o->u.list.len++;
}

py_value py_list_get(py_value list, int32_t index)
{
    if (list.type != PY_LIST)
        return py_none();
    py_obj *o = list.u.obj;
    if (index < 0)
        index += (int32_t)o->u.list.len;
    if (index < 0 || index >= (int32_t)o->u.list.len)
        return py_none();
    return o->u.list.items[index];
}

void py_list_set(py_value list, int32_t index, py_value v)
{
    if (list.type != PY_LIST)
        return;
    py_obj *o = list.u.obj;
    if (index < 0)
        index += (int32_t)o->u.list.len;
    if (index < 0 || index >= (int32_t)o->u.list.len)
        return;
    py_decref(o->u.list.items[index]);
    o->u.list.items[index] = v;
}

py_value py_list_pop(py_value list, int32_t index)
{
    if (list.type != PY_LIST)
        return py_none();
    py_obj *o = list.u.obj;
    if (o->u.list.len == 0)
        return py_none();
    if (index < 0)
        index += (int32_t)o->u.list.len;
    if (index < 0 || index >= (int32_t)o->u.list.len)
        return py_none();
    py_value v = o->u.list.items[index];
    for (uint32_t i = (uint32_t)index; i + 1 < o->u.list.len; i++)
        o->u.list.items[i] = o->u.list.items[i + 1];
    o->u.list.len--;
    return v;
}

/* ---- dicts ------------------------------------------------------------ */

/* A dict is an insertion-ordered array.  See py_obj.h for why the order is
 * worth more than the lookup speed here.
 *
 * The array grows by doubling and never shrinks: a script that deletes
 * from a dict does not usually do it in a loop, and keeping the capacity
 * means a delete does not have to copy the tail. */

static int dict_reserve(py_obj *o, uint32_t want)
{
    if (want <= o->u.dict.cap)
        return 0;
    uint32_t cap = o->u.dict.cap ? o->u.dict.cap : 4;
    while (cap < want)
        cap *= 2;
    py_dict_entry *e = (py_dict_entry *)py_alloc(cap * (uint32_t)sizeof(py_dict_entry));
    if (!e)
        return -1;
    if (o->u.dict.entries) {
        memcpy(e, o->u.dict.entries, o->u.dict.count * sizeof(py_dict_entry));
        py_free_auto(o->u.dict.entries);
    }
    o->u.dict.entries = e;
    o->u.dict.cap = cap;
    return 0;
}

/* The index of this key, or -1. */
static int32_t dict_find(py_obj *o, py_value key)
{
    for (uint32_t i = 0; i < o->u.dict.count; i++) {
        if (py_eq(o->u.dict.entries[i].key, key))
            return (int32_t)i;
    }
    return -1;
}

int py_dict_get(py_value dict, py_value key, py_value *out)
{
    if (dict.type != PY_DICT)
        return 0;
    py_obj *o = dict.u.obj;
    int32_t idx = dict_find(o, key);
    if (idx < 0)
        return 0;
    if (out)
        *out = o->u.dict.entries[idx].val;
    return 1;
}

/* Set a key.  The dict takes over the references to both the key and the
 * value. */
void py_dict_set(py_value dict, py_value key, py_value val)
{
    if (dict.type != PY_DICT)
        return;
    py_obj *o = dict.u.obj;

    int32_t idx = dict_find(o, key);
    if (idx >= 0) {
        /* Replace: the dict takes the new reference, the old one goes. */
        py_decref(o->u.dict.entries[idx].val);
        o->u.dict.entries[idx].val = val;
        return;
    }

    if (dict_reserve(o, o->u.dict.count + 1) != 0)
        return;
    o->u.dict.entries[o->u.dict.count].key = key;
    o->u.dict.entries[o->u.dict.count].val = val;
    o->u.dict.count++;
}

int py_dict_del(py_value dict, py_value key)
{
    if (dict.type != PY_DICT)
        return 0;
    py_obj *o = dict.u.obj;
    int32_t idx = dict_find(o, key);
    if (idx < 0)
        return 0;

    py_decref(o->u.dict.entries[idx].key);
    py_decref(o->u.dict.entries[idx].val);
    /* Close the gap: the order of the other keys does not change. */
    for (uint32_t i = (uint32_t)idx; i + 1 < o->u.dict.count; i++)
        o->u.dict.entries[i] = o->u.dict.entries[i + 1];
    o->u.dict.count--;
    return 1;
}

int32_t py_dict_len(py_value dict)
{
    if (dict.type != PY_DICT)
        return 0;
    return (int32_t)dict.u.obj->u.dict.count;
}

/* ---- rendering --------------------------------------------------------- */

/* A growable byte buffer, used to build a repr before it becomes a string.
 * Containers nest, so the text is built bottom up rather than with a fixed
 * scratch array. */
typedef struct sbuf {
    char *buf;
    uint32_t len;
    uint32_t cap;
    int failed;
} sbuf;

static void sb_init(sbuf *s)
{
    s->buf = 0;
    s->len = 0;
    s->cap = 0;
    s->failed = 0;
}

static int sb_reserve(sbuf *s, uint32_t extra)
{
    if (s->failed)
        return -1;
    if (s->len + extra + 1 <= s->cap)
        return 0;
    uint32_t cap = s->cap ? s->cap : 64;
    while (cap < s->len + extra + 1)
        cap *= 2;
    char *nb = (char *)py_alloc(cap);
    if (!nb) {
        s->failed = 1;
        return -1;
    }
    if (s->buf) {
        memcpy(nb, s->buf, s->len);
        py_free_auto(s->buf);
    }
    s->buf = nb;
    s->cap = cap;
    return 0;
}

static void sb_put(sbuf *s, const char *p, uint32_t n)
{
    if (sb_reserve(s, n) != 0)
        return;
    memcpy(s->buf + s->len, p, n);
    s->len += n;
    s->buf[s->len] = 0;
}

static void sb_putc(sbuf *s, char c)
{
    sb_put(s, &c, 1);
}

static void sb_puts(sbuf *s, const char *p)
{
    sb_put(s, p, (uint32_t)strlen(p));
}

/* Format a signed 32-bit integer.  INT32_MIN is negated in unsigned
 * arithmetic, because negating it as a signed value overflows. */
void py_format_int(char *buf, uint32_t cap, int32_t v)
{
    char tmp[12];
    int n = 0;
    uint32_t u = (v < 0) ? (0u - (uint32_t)v) : (uint32_t)v;

    do {
        tmp[n++] = (char)('0' + (u % 10u));
        u /= 10u;
    } while (u && n < 11);
    if (v < 0)
        tmp[n++] = '-';

    uint32_t out = 0;
    if (cap == 0)
        return;
    while (n > 0 && out + 1 < cap)
        buf[out++] = tmp[--n];
    buf[out] = 0;
}

/* Render a value into the buffer.  `quoted` distinguishes repr() (strings
 * get quotes) from str() (they do not). */
static void render(sbuf *s, py_value v, int quoted)
{
    switch (v.type) {
    case PY_NONE:
        sb_puts(s, "None");
        return;
    case PY_BOOL:
        sb_puts(s, v.u.i ? "True" : "False");
        return;
    case PY_INT: {
        char tmp[12];
        py_format_int(tmp, sizeof(tmp), v.u.i);
        sb_puts(s, tmp);
        return;
    }
    case PY_STR: {
        py_str *str = &v.u.obj->u.str;
        if (quoted) {
            sb_putc(s, '\'');
            for (uint32_t i = 0; i < str->len; i++) {
                char c = str->bytes[i];
                if (c == '\'' || c == '\\')
                    sb_putc(s, '\\');
                sb_putc(s, c);
            }
            sb_putc(s, '\'');
        } else {
            sb_put(s, str->bytes, str->len);
        }
        return;
    }
    case PY_LIST: {
        py_list *l = &v.u.obj->u.list;
        sb_putc(s, '[');
        for (uint32_t i = 0; i < l->len; i++) {
            if (i)
                sb_puts(s, ", ");
            render(s, l->items[i], 1);
        }
        sb_putc(s, ']');
        return;
    }
    case PY_DICT: {
        py_dict *d = &v.u.obj->u.dict;
        sb_putc(s, '{');
        for (uint32_t i = 0; i < d->count; i++) {
            if (i)
                sb_puts(s, ", ");
            render(s, d->entries[i].key, 1);
            sb_puts(s, ": ");
            render(s, d->entries[i].val, 1);
        }
        sb_putc(s, '}');
        return;
    }
    case PY_FUNC: {
        sb_puts(s, "<function ");
        sb_puts(s, v.u.obj->u.func.name ? v.u.obj->u.func.name : "?");
        sb_putc(s, '>');
        return;
    }
    case PY_BUILTIN: {
        sb_puts(s, "<built-in function ");
        sb_puts(s, v.u.obj->u.builtin.name ? v.u.obj->u.builtin.name : "?");
        sb_putc(s, '>');
        return;
    }
    }
}

py_value py_repr(py_value v)
{
    sbuf s;
    sb_init(&s);
    render(&s, v, 1);
    if (s.failed) {
        if (s.buf)
            py_free_auto(s.buf);
        return py_new_str("<out of memory>");
    }
    py_value r = py_new_strn(s.buf ? s.buf : "", s.len);
    if (s.buf)
        py_free_auto(s.buf);
    return r;
}

py_value py_str_of(py_value v)
{
    sbuf s;
    sb_init(&s);
    render(&s, v, 0);
    if (s.failed) {
        if (s.buf)
            py_free_auto(s.buf);
        return py_new_str("<out of memory>");
    }
    py_value r = py_new_strn(s.buf ? s.buf : "", s.len);
    if (s.buf)
        py_free_auto(s.buf);
    return r;
}
