#ifndef TINYOS_FILE_H
#define TINYOS_FILE_H

#include "types.h"

/* One open descriptor's state, and the descriptor table's size.
 *
 * This lives in a header of its own because two translation units now share
 * it: process.c keeps the table in struct proc, and fd.c owns everything
 * that changes an entry.  MAX_FDS is the table length in both.
 *
 * Nothing here knows about a task or a scheduler; the hooks that do live in
 * kernel/fd_internal.h.
 */

#define MAX_FDS 12

struct pipe;
struct dev;

/* What a descriptor points at.  A pipe needs its own type because the two
 * ends behave differently (a read end sees EOF, a write end sees EPIPE), and
 * console descriptors are marked rather than left implicit so dup2() can
 * replace one: fd 1 is FD_CONSOLE until somebody redirects it. */
enum {
    FD_NONE = 0,
    FD_CONSOLE,
    FD_FILE,
    FD_PIPE_R,
    FD_PIPE_W,
    FD_CHRDEV,      /* /dev/... or /proc/...: a table of ops, not a file */
};

typedef struct {
    uint8_t  type;
    uint8_t  writable;     /* FD_FILE: created for writing (reopen as rw) */
    int      fh;           /* FD_FILE: littlefs open-file handle */
    uint32_t size;
    uint32_t pos;
    union {
        struct pipe *pipe; /* FD_PIPE_R / FD_PIPE_W */
        struct {
            const struct dev *dev;  /* FD_CHRDEV: which device */
            void *devpriv;     /* FD_CHRDEV: per-open state from ops->open() */
        } chr;
        char *path;        /* FD_FILE: heap copy, so a child can reopen it */
    } u;
} file_t;

#endif /* TINYOS_FILE_H */
