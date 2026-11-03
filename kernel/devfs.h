#ifndef TINYOS_DEVFS_H
#define TINYOS_DEVFS_H

#include "types.h"

/* Device files: /dev/gpio4, /dev/adc2, /proc/ps ...
 *
 * The point is not "Unix compatibility", it is that a device you can open is
 * a device a shell script can use: `cat /dev/adc2` and `echo 1 > /dev/gpio4`
 * replace a purpose-built C program per peripheral.  The C API (gpio_read()
 * and friends) stays, because three syscalls plus a path lookup is the wrong
 * way to toggle a pin in a loop - see the note on read/write below.
 *
 * A driver fills in `struct dev_ops`; nothing else in the kernel knows what a
 * device is.  process.c holds the descriptor and calls through the table, the
 * same way it already called console_write() for FD_CONSOLE - the only change
 * is that the special case became a lookup. */

struct dev;

struct dev_ops {
    /* Optional.  Called once per open() to build per-descriptor state (a
     * /proc snapshot buffer, say).  Without it the descriptor uses the
     * device's own `priv`, which is all a stateless device needs. */
    int  (*open)(const struct dev *d, void **priv);

    /* `off` is the descriptor's read position.  Streaming devices ignore it;
     * snapshot devices use it to answer "already read everything" with 0,
     * which is what makes `cat` terminate instead of looping forever. */
    int  (*read)(void *priv, uint32_t off, void *buf, uint32_t len);

    /* Returns bytes accepted, or -1.  A short count means "the rest was not
     * consumed" - the shell's echo writes "1\n" and a pin driver takes the
     * first digit and calls it a day. */
    int  (*write)(void *priv, uint32_t off, const void *buf, uint32_t len);

    /* Configuration that is neither a read nor a write: pin direction, ADC
     * reference voltage.  This is why the kernel did not need sysfs - a
     * command plus an integer argument covers what a tree of one-value files
     * covers, at a fraction of the cost. */
    int  (*ioctl)(void *priv, int cmd, int arg);

    /* Optional.  Releases what open() built. */
    void (*close)(void *priv);
};

typedef struct dev {
    const char      *name;   /* path without the leading slash: "dev/gpio4" */
    const struct dev_ops *ops;
    void            *priv;   /* driver data handed to every op (pin number) */
} dev_t;

/* Registration happens at boot, before any task runs; the table is read-only
 * afterwards, which is why open() needs no lock on an SMP build. */
void        devfs_init(void);
int         devfs_register(const char *name, const struct dev_ops *ops,
                           void *priv);
/* "/dev/gpio4", "dev/gpio4" and "/proc/ps" all resolve.  NULL when the path
 * is not a device - that is the caller's cue to try the filesystem. */
const dev_t *devfs_lookup(const char *path);

/* Directory enumeration, what readdir needs.  There is no directory object:
 * a registered name is "dev/gpio4", so a directory is the prefix before the
 * last slash, and the root's entries are the distinct first components of
 * the table ("dev", "proc", ...).  Deriving them instead of hard-coding a
 * list keeps a future namespace (net/) working without touching this file.
 *
 * Returns -1 when `path` is not a devfs directory - that is the caller's cue
 * to ask littlefs - and 0 or more for a directory that is ours (0 meaning
 * "ours and empty", which is still not a littlefs path). */
int         devfs_dir_count(const char *path);

/* Index-th bare entry name ("gpio4" inside "/dev") copied into `buf`, so the
 * caller never holds a pointer into the table.  Same contract as
 * ramdisk_list_name(): length, 0 past the end, -1 if it does not fit. */
int         devfs_dir_name(const char *path, uint32_t index, char *buf,
                           int cap);

#endif
