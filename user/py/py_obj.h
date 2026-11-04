/* py_obj.h - the interpreter's values.
 *
 * A value is a tagged union: an int, a bool, None, or a pointer to a heap
 * object (string, list, dict).  Ints are not heap objects: they are values
 * in the union, so arithmetic never allocates and never needs a free.
 *
 * Heap objects are reference counted.  A list holds references to its
 * elements, a dict to its keys and values, and a scope to the values of
 * its names.  When the last reference goes, the object is released and its
 * own references drop, recursively.
 *
 * Reference cycles are not collected.  A list that contains itself leaks.
 * That is a deliberate limit, not an oversight: a cycle collector needs a
 * root set and a mark phase, which is the expensive half of a garbage
 * collector, and this interpreter's scripts are short-lived - the process
 * exits when the script ends and the kernel gives the whole window back.
 */

#ifndef TINYOS_PY_OBJ_H
#define TINYOS_PY_OBJ_H

#include "types.h"
#include "py_mem.h"

typedef enum {
    PY_NONE = 0,
    PY_INT,
    PY_BOOL,
    PY_STR,
    PY_LIST,
    PY_DICT,
    PY_FUNC,
    PY_BUILTIN
} py_type;

typedef struct py_obj py_obj;
typedef struct py_value py_value;
typedef struct py_env py_env;
typedef struct py_node py_node;

/* A call in progress.  Builtins take one of these rather than a plain
 * argument array, because a builtin may want keyword arguments too
 * (print(x, sep="-")).  Defined in py_eval.h. */
struct py_call;

/* A function object: a name, an argument count, and a body to run.  The
 * body is a scope node; see py_ast.h.  `closure` is the environment the
 * def appeared in, so a function defined inside another function still
 * sees the names around it.  `def` is the syntax tree node the function
 * came from - it holds the parameter names, which the caller needs and
 * which are not worth copying into every function object. */
typedef struct py_func {
    char *name;
    int argc;
    struct py_scope *body;
    py_env *closure;
    py_node *def;
} py_func;

/* A string: bytes and a length.  Strings are immutable and their bytes are
 * NUL terminated so they can go straight to write() and printf(). */
typedef struct py_str {
    uint32_t len;
    char *bytes;
} py_str;

/* A list: a growable array of values, plus the capacity so the next
 * append does not have to reallocate every time. */
typedef struct py_list {
    uint32_t len;
    uint32_t cap;
    py_value *items;
} py_list;

/* A dict: an insertion-ordered array of (key, value) pairs.
 *
 * The order is the one Python has had since 3.7: the keys come out in the
 * order they went in.  A hash table would be faster for a big dict, but it
 * would also put the keys in hash order, which is not what a script means
 * when it prints them - and an array of a few dozen pairs is a lookup the
 * size of a cache line.  For a microcontroller, the order is worth more
 * than the asymptotics.
 *
 * Keys are strings or ints; anything else is a type error, the same as
 * Python. */
typedef struct py_dict_entry py_dict_entry;

typedef struct py_dict {
    uint32_t count;      /* live entries */
    uint32_t cap;        /* capacity of the entries array */
    py_dict_entry *entries;
} py_dict;

/* The value type comes before the object type, because a dict entry holds
 * a key and a value by value, and the object type holds the containers by
 * value.  The order below is the only one that works: the containers first
 * (they only point at things), then the value, then the entry that needs
 * the value, then the object that holds the containers. */
struct py_value {
    py_type type;
    union {
        int32_t i;
        py_obj *obj;
    } u;
};

struct py_dict_entry {
    py_value key;
    py_value val;
};

struct py_obj {
    uint32_t refs;
    py_type type;
    union {
        py_str str;
        py_list list;
        py_dict dict;
        py_func func;
        struct {
            const char *name;
            py_value (*fn)(struct py_call *c);
        } builtin;
    } u;
};

/* Constructors.  Each returns a value that owns one reference to its
 * object; the caller passes it on (into a list, a scope) or releases it. */
py_value py_int(int32_t v);
py_value py_bool(int v);
py_value py_none(void);
py_value py_new_str(const char *s);
py_value py_new_strn(const char *s, uint32_t len);
py_value py_new_list(void);
py_value py_new_dict(void);

/* Reference counting. */
void py_incref(py_value v);
void py_decref(py_value v);

/* Truthiness, the way Python defines it: 0, "", empty containers and
 * None are false, everything else is true. */
int py_truthy(py_value v);

/* Equality.  Numbers compare by value across int and bool; strings by
 * content; lists and dicts by content too, the way Python compares them:
 * two lists holding the same things are equal, and two dicts with the
 * same keys and values are equal. */
int py_eq(py_value a, py_value b);

/* Hashing, for dict keys.  Strings hash by content, ints by value. */
uint32_t py_hash(py_value v);

/* List operations. */
void py_list_append(py_value list, py_value item);
py_value py_list_get(py_value list, int32_t index);
void py_list_set(py_value list, int32_t index, py_value v);
int32_t py_list_len(py_value list);
void py_list_insert(py_value list, int32_t index, py_value v);
py_value py_list_pop(py_value list, int32_t index);

/* Dict operations.  py_dict_get returns 1 and sets *out when the key is
 * there, 0 otherwise. */
int py_dict_get(py_value dict, py_value key, py_value *out);
void py_dict_set(py_value dict, py_value key, py_value val);
int py_dict_del(py_value dict, py_value key);
int32_t py_dict_len(py_value dict);

/* Rendering, the way print() and str() do it.  The result is a string
 * value the caller owns. */
py_value py_repr(py_value v);
py_value py_str_of(py_value v);

/* The type name, for type() and for error messages. */
const char *py_type_name(py_value v);

/* Number formatting, used by repr() and by the f-string-ish formatting in
 * py_print. */
void py_format_int(char *buf, uint32_t cap, int32_t v);

#endif
