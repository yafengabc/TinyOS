#include "devfs.h"
#include "string.h"

/* A flat table, sized for the biggest board rather than grown at runtime: the
 * RP2040 exposes 30 pins and 5 ADC channels and the RP2350 will expose more,
 * so the ceiling is generous but the storage is trivial (64 entries = 768 B
 * of .bss, less than one percent of what a single task window costs).
 *
 * Names are copied into a static pool because drivers want to build them in a
 * loop ("gpio" + itoa(i)) and the table must not point at a local buffer. */
/* 32 slots is plenty now that pin families are templates: the RP2350 uses 13
 * (7 in /dev - led0, key0, lcd0, screen, fb0, gpio, adc - and 6 in /proc).
 * Before templates it needed 46, thirty of them just for /dev/gpio0..29, and
 * a 48-pin part would have overflowed the old 64. */
#define MAX_DEVS     32
#define DEV_NAME_MAX 16

static dev_t devs[MAX_DEVS];
static char  names[MAX_DEVS][DEV_NAME_MAX];
static int   ndevs;
static int   dropped;   /* refused because the table was full, see below */

/* Numeric families ("dev/gpio" + index).  Kept beside the table rather than
 * in it: a template needs a resolver, which a dev_t has no room for. */
#define MAX_TMPL 4
static struct {
    const char        *prefix;
    int                count;
    devfs_tmpl_resolve resolve;
} tmpl[MAX_TMPL];
static int   ntmpl;

void devfs_init(void)
{
    ndevs = 0;
    ntmpl = 0;
    dropped = 0;
}

int devfs_register(const char *name, const struct dev_ops *ops, void *priv)
{
    uint32_t n;

    if (!name || !ops)
        return -1;
    if (ndevs >= MAX_DEVS) {
        /* Count it instead of failing quietly: the first symptom of a full
         * table is a missing /dev/fb0 much later, and that is a long walk
         * back to this line.  hwdev_init() prints the count at boot. */
        dropped++;
        return -1;
    }
    n = (uint32_t)strlen(name);
    if (n == 0 || n >= DEV_NAME_MAX)
        return -1;

    /* Silently keeping the first registration would make a name collision
     * depend on link order; refusing it makes the bug show up at boot. */
    for (int i = 0; i < ndevs; i++)
        if (!strcmp(devs[i].name, name))
            return -1;

    memcpy(names[ndevs], name, n + 1);
    devs[ndevs].name = names[ndevs];
    devs[ndevs].ops  = ops;
    devs[ndevs].priv = priv;
    return ndevs++;
}

/* How many registrations the table refused.  Non-zero means MAX_DEVS is too
 * small for this board; hwdev_init() reports it at boot. */
int devfs_dropped(void)
{
    return dropped;
}

int devfs_register_template(const char *prefix, int count,
                            devfs_tmpl_resolve resolve)
{
    if (!prefix || !*prefix || !resolve || count <= 0)
        return -1;
    if (ntmpl >= MAX_TMPL || strlen(prefix) >= DEV_NAME_MAX)
        return -1;

    tmpl[ntmpl].prefix  = prefix;
    tmpl[ntmpl].count   = count;
    tmpl[ntmpl].resolve = resolve;
    return ntmpl++;
}

/* Does the template hang directly below `dir`?  "dev/gpio" is a child of
 * "dev", not of "dev/gpio0". */
static int tmpl_child(const char *prefix, const char *dir)
{
    size_t n = strlen(dir);

    if (strncmp(prefix, dir, n) != 0 || prefix[n] != '/')
        return 0;
    return strchr(prefix + n + 1, '/') == NULL;
}

const dev_t *devfs_lookup(const char *path)
{
    if (!path)
        return NULL;

    /* A caller normally hands over an absolute path ("/dev/gpio4"); the
     * rest of the kernel stores names without the slash. */
    while (*path == '/')
        path++;

    for (int i = 0; i < ndevs; i++)
        if (!strcmp(devs[i].name, path))
            return &devs[i];

    /* Numeric families: "dev/gpio17" resolves through the "dev/gpio"
     * template.  The exact table is searched first so a board can still
     * single out one instance if it ever needs to. */
    for (int i = 0; i < ntmpl; i++) {
        size_t n = strlen(tmpl[i].prefix);
        const char *s;
        int v = 0;

        if (strncmp(path, tmpl[i].prefix, n) != 0)
            continue;
        s = path + n;
        /* The family name on its own means instance 0: it is what `ls dev`
         * lists and what `cat /dev/gpio` opens, and pin 0 is as good a
         * default as any other. */
        if (*s == '\0')
            return tmpl[i].resolve(0);
        if (*s < '0' || *s > '9')
            continue;
        while (*s >= '0' && *s <= '9' && v < tmpl[i].count)
            v = v * 10 + (*s++ - '0');
        if (*s || v >= tmpl[i].count)   /* trailing junk, or out of range */
            continue;
        return tmpl[i].resolve(v);
    }
    return NULL;
}

/* ---- directory view ---------------------------------------------------- */

/* A path arriving here is absolute ("/dev/gpio4") or not ("dev/gpio4"); the
 * table stores the latter. */
static const char *strip_slash(const char *p)
{
    if (!p)
        return "";
    while (*p == '/')
        p++;
    return p;
}

/* Is `name` a direct child of `dir`?  Returns the part after "dir/", or NULL
 * if the name is not one level below it.  "gpio4/deep" is a grandchild and
 * therefore not an entry of /dev - the table has no subdirectories today,
 * but a driver that builds "net/eth0" into the name should not leak into
 * `ls /`. */
static const char *child_of(const char *name, const char *dir)
{
    size_t n = strlen(dir);
    const char *rest;

    if (strncmp(name, dir, n) != 0 || name[n] != '/')
        return NULL;
    rest = name + n + 1;
    if (!*rest || strchr(rest, '/'))
        return NULL;
    return rest;
}

/* Length of the first path component of `name` ("dev" in "dev/gpio4"). */
static size_t head_len(const char *name)
{
    const char *p = strchr(name, '/');

    return p ? (size_t)(p - name) : strlen(name);
}

/* The namespaces the kernel routes here on every open() exist even on a board
 * that registered nothing inside them.  /dev holding zero device files is a
 * fact about the board; `cd /dev` failing would turn that fact into a
 * different shell behaviour per target, which is the kind of difference this
 * layer exists to hide. */
static const char *const ns_dirs[] = { "dev", "proc" };
#define NS_COUNT ((int)(sizeof(ns_dirs) / sizeof(ns_dirs[0])))

static int is_ns(const char *dir)
{
    for (int i = 0; i < NS_COUNT; i++)
        if (!strcmp(ns_dirs[i], dir))
            return 1;
    return 0;
}

static int head_is_ns(const char *name, size_t hl)
{
    for (int i = 0; i < NS_COUNT; i++)
        if (strlen(ns_dirs[i]) == hl && !strncmp(name, ns_dirs[i], hl))
            return 1;
    return 0;
}

/* Does `dir` hold anything?  For a name that is not one of the fixed
 * namespaces this is also the test for "is this a devfs directory at all":
 * nothing registered below it means it is a littlefs path that merely looks
 * similar. */
static int dir_has_entries(const char *dir)
{
    for (int i = 0; i < ndevs; i++)
        if (child_of(devs[i].name, dir))
            return 1;
    for (int i = 0; i < ntmpl; i++)
        if (tmpl_child(tmpl[i].prefix, dir))
            return 1;
    return 0;
}

/* Is devs[i] the first entry under a head that the root has not listed yet?
 * That is what turns 22 "dev/gpioN" rows into one "dev" in `ls /`. */
static int first_extra_head(int i)
{
    size_t hl = head_len(devs[i].name);

    if (head_is_ns(devs[i].name, hl))
        return 0;
    for (int j = 0; j < i; j++)
        if (head_len(devs[j].name) == hl &&
            !strncmp(devs[j].name, devs[i].name, hl))
            return 0;
    return 1;
}

/* The root's entries are the fixed namespaces followed by the distinct first
 * components of the table, in registration order. */
static int root_count(void)
{
    int n = NS_COUNT;

    for (int i = 0; i < ndevs; i++)
        if (first_extra_head(i))
            n++;
    return n;
}

int devfs_dir_count(const char *path)
{
    const char *dir = strip_slash(path);
    int n = 0;

    if (!*dir)
        return root_count();
    if (!is_ns(dir) && !dir_has_entries(dir))
        return -1;              /* not ours: let the caller try littlefs */
    for (int i = 0; i < ndevs; i++)
        if (child_of(devs[i].name, dir))
            n++;
    /* A template is one row in the listing ("gpio"), not one per instance:
     * the family is what exists, the numbers on it are not separate nodes. */
    for (int i = 0; i < ntmpl; i++)
        if (tmpl_child(tmpl[i].prefix, dir))
            n++;
    return n;
}

/* Copy one name into the caller's buffer.  -1 means it does not fit, which
 * readdir reports as end-of-directory rather than handing back a stub. */
static int put_name(char *buf, int cap, const char *name, size_t len)
{
    if (cap <= 0 || len >= (size_t)cap)
        return -1;
    memcpy(buf, name, len);
    buf[len] = '\0';
    return (int)len;
}

int devfs_dir_name(const char *path, uint32_t index, char *buf, int cap)
{
    const char *dir = strip_slash(path);

    if (!*dir) {
        /* Root: the fixed namespaces, then the distinct heads of the table. */
        uint32_t extra;

        if (index < (uint32_t)NS_COUNT)
            return put_name(buf, cap, ns_dirs[index], strlen(ns_dirs[index]));
        extra = index - (uint32_t)NS_COUNT;
        for (int i = 0; i < ndevs; i++) {
            if (!first_extra_head(i))
                continue;
            if (extra-- != 0)
                continue;
            return put_name(buf, cap, devs[i].name, head_len(devs[i].name));
        }
        return 0;
    }

    for (int i = 0; i < ndevs; i++) {
        const char *rest = child_of(devs[i].name, dir);

        if (!rest)
            continue;
        if (index-- == 0)
            return put_name(buf, cap, rest, strlen(rest));
    }
    for (int i = 0; i < ntmpl; i++) {
        const char *rest;

        if (!tmpl_child(tmpl[i].prefix, dir))
            continue;
        rest = tmpl[i].prefix + strlen(dir) + 1;
        if (index-- == 0)
            return put_name(buf, cap, rest, strlen(rest));
    }
    return 0;
}
