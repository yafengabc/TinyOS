/* Descriptor table, file objects and the fd syscalls.
 *
 * Split out of process.c (docs/ARCHITECTURE-REVIEW-2026-11-22.md P0-2).  What
 * stayed behind is everything that has to touch a task: read()/write() and
 * the pipe blocking, plus the hooks declared in fd_internal.h.  This file
 * never dereferences struct proc or struct thread - PFD(), group_lock() and
 * current below expand to process.c accessors, so the dependency runs one
 * way and the table's owner stays the scheduler half.
 */
#include "fd_internal.h"
#include "process.h"
#include "ramdisk.h"
#include "devfs.h"
#include "heap.h"
#include "pipe.h"
#include "tty.h"
#include "string.h"
#include "platform.h"
#include "tos.h"

/* The code moved here verbatim says current, PFD(p), group_lock(p);
 * the three macros below turn those into the process.c accessors, so this
 * file never reaches into struct proc or struct thread. */
#define current         fd_current()
#define PFD(p)          proc_fd_table(p)
#define group_lock(p)   proc_fd_lock(p)
#define group_unlock(p) proc_fd_unlock(p)

static struct thread *fd_current(void)
{
    return (struct thread *)process_current_task();
}

/* ---- file descriptors ------------------------------------------------- */

file_t *fd_slot(struct thread *p, int fd)
{
    /* A kernel thread has no process and therefore no descriptor table.  The
     * null check is not decoration: proc_retire() closes every slot of a task
     * whose exec() never got a process entry, and indexing the NULL table read
     * twelve words below the first page - seen on the C3 emulator as
     * "[bus] unmapped read @0x18" right after "process or thread table full".
     * A missing table means "no such descriptor", which is what every caller
     * already handles. */
    if (!p || !PFD(p) || fd < 0 || fd >= MAX_FDS)
        return NULL;
    return &PFD(p)[fd];
}

/* The descriptor number *is* the table index.  open() starts at 3: 0/1/2
 * belong to the console and a program that opens a file must not be handed
 * its own stdout by mistake. */
static int fd_alloc(struct thread *p)
{
    for (int i = 3; i < MAX_FDS; i++)
        if (PFD(p)[i].type == FD_NONE)
            return i;
    return -1;
}

/* A path has to survive the descriptor if a child is ever going to reopen it
 * (see the inheritance path in proc_create).  Copying is the only option: the
 * caller's buffer is a syscall argument that is gone by the time a child
 * exists.  Failure is not fatal - the file simply becomes non-inheritable,
 * which is what every file used to be. */
static char *kstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = kmalloc(n);

    if (p)
        memcpy(p, s, n);
    return p;
}

/* Make `dst` a second, independent instance of `src`.
 *
 * The rule that shapes this function: a descriptor owns something, and two
 * descriptors must never own it twice.  A pipe end is shared by refcount; a
 * file handle is *reopened* because it carries a read position and exactly
 * one close; a character device gets its own per-open state so /proc buffers
 * are not freed twice.
 *
 * Returns 0, or -1 when the source cannot be duplicated (a file whose path
 * was not saved, because the heap was full when it was opened). */
int fd_clone(file_t *dst, const file_t *src)
{
    if (!dst || !src)
        return -1;
    memset(dst, 0, sizeof(*dst));

    switch (src->type) {
    case FD_CONSOLE:
        dst->type = FD_CONSOLE;
        return 0;

    case FD_PIPE_R:
    case FD_PIPE_W:
        *dst = *src;
        if (src->type == FD_PIPE_R)
            pipe_ref_reader(src->u.pipe);
        else
            pipe_ref_writer(src->u.pipe);
        return 0;

    case FD_FILE: {
        int h;

        if (!src->u.path)
            return -1;
        /* Reopen with the same access the original had.  A read-only handle
         * would make `echo hi > f` fail in the child, which is the one case
         * inheritance exists for now. */
        h = src->writable ? ramdisk_open_rw(src->u.path)
                          : ramdisk_open(src->u.path);
        if (h < 0)
            return -1;
        /* The new handle starts at 0; the descriptor it replaces was
         * somewhere else.  Without this a child reading a file it inherited
         * halfway through would start over. */
        if (ramdisk_seek(h, src->pos) < 0) {
            ramdisk_close(h);
            return -1;
        }
        dst->type     = FD_FILE;
        dst->fh       = h;
        dst->size     = ramdisk_size(h);
        dst->pos      = src->pos;
        dst->writable = src->writable;
        dst->u.path     = kstrdup(src->u.path);
        return 0;
    }

    case FD_CHRDEV: {
        void *priv = src->u.chr.dev->priv;

        if (src->u.chr.dev->ops->open && src->u.chr.dev->ops->open(src->u.chr.dev, &priv) < 0)
            return -1;
        dst->type    = FD_CHRDEV;
        dst->u.chr.dev     = src->u.chr.dev;
        dst->u.chr.devpriv = priv;
        dst->pos     = src->pos;
        return 0;
    }

    default:
        return -1;
    }
}

int process_fd_open(const char *path)
{
    const dev_t *d;
    file_t *f;
    int fd, h;

    if (!current)
        return -1;

    /* Device paths are not on the filesystem, so this lookup failing is the
     * normal case for every plain file open.  Trying devices first costs one
     * strcmp chain and keeps ramdisk.c from needing to know about /dev. */
    d = devfs_lookup(path);
    if (d) {
        void *priv = d->priv;

        group_lock(current);
        fd = fd_alloc(current);
        if (fd < 0) {
            group_unlock(current);
            return -1;
        }
        if (d->ops->open && d->ops->open(d, &priv) < 0) {
            PFD(current)[fd].type = FD_NONE;
            group_unlock(current);
            return -1;
        }
        f = &PFD(current)[fd];
        f->type    = FD_CHRDEV;
        f->u.chr.dev     = d;
        f->u.chr.devpriv = priv;
        f->pos     = 0;
        f->size    = 0;
        group_unlock(current);
        return fd;
    }

    h = ramdisk_open(path);
    if (h < 0)
        return -1;

    group_lock(current);
    fd = fd_alloc(current);
    if (fd < 0) {
        group_unlock(current);
        ramdisk_close(h);
        return -1;
    }
    f = &PFD(current)[fd];
    f->type = FD_FILE;
    f->fh   = h;
    f->size = ramdisk_size(h);
    f->pos  = 0;
    f->u.path = kstrdup(path);
    group_unlock(current);
    return fd;
}

int process_fd_create(const char *path)
{
    file_t *f;
    int fd, h;

    if (!current)
        return -1;

    /* Creating a device node is a boot-time act (devfs_register), not
     * something a task does by opening a path for writing. */
    if (devfs_lookup(path))
        return -1;

    h = ramdisk_create(path);
    if (h < 0)
        return -1;

    group_lock(current);
    fd = fd_alloc(current);
    if (fd < 0) {
        group_unlock(current);
        ramdisk_close(h);
        return -1;
    }
    f = &PFD(current)[fd];
    f->type = FD_FILE;
    f->fh   = h;
    f->size = ramdisk_size(h);
    f->pos  = 0;
    f->u.path = kstrdup(path);
    f->writable = 1;            /* a child inheriting this must be able to write */
    group_unlock(current);
    return fd;
}


/* Close one descriptor of `p`.  Shared by the syscall (the caller is `p`)
 * and by task_release (a dead task, which is not necessarily current). */
void fd_close_in(struct thread *p, int fd)
{
    file_t *f = fd_slot(p, fd);

    if (!f || f->type == FD_NONE)
        return;

    switch (f->type) {
    case FD_FILE:
        ramdisk_close(f->fh);
        if (f->u.path)
            kfree(f->u.path);
        break;

    case FD_CHRDEV:
        if (f->u.chr.dev->ops->close)
            f->u.chr.dev->ops->close(f->u.chr.devpriv);
        break;

    /* Dropping the reference and deciding who to wake is one step in
     * process_pipe_closed(): the last reader going away is what turns "empty
     * but writers exist" into "no data will ever arrive", and a writer that
     * checked in between would miss it. */
    case FD_PIPE_R:
        process_pipe_closed(f->u.pipe, 0);
        break;

    case FD_PIPE_W:
        process_pipe_closed(f->u.pipe, 1);
        break;

    default:
        break;
    }

    memset(f, 0, sizeof(*f));
    /* 0, 1 and 2 are never really closed: "close the redirect" means "back
     * to the console", which is what the shell relies on after a pipeline
     * puts fd 1 on a pipe. */
    if (fd < 3)
        f->type = FD_CONSOLE;
}

int process_fd_close(int fd)
{
    if (!current)
        return -1;
    if (fd < 0 || fd >= MAX_FDS)
        return -1;
    group_lock(current);
    fd_close_in(current, fd);
    group_unlock(current);
    return 0;
}

/* Configuration on an open descriptor: the half of device access that is
 * neither a read nor a write.  Only character devices have an answer; for a
 * file or a pipe "set the mode" has no meaning, and -1 says so instead of
 * silently succeeding. */
int process_ioctl(int fd, int cmd, int arg)
{
    file_t *f;

    if (!current)
        return -1;
    f = fd_slot(current, fd);
    if (!f || f->type != FD_CHRDEV || !f->u.chr.dev->ops->ioctl)
        return -1;
    return f->u.chr.dev->ops->ioctl(f->u.chr.devpriv, cmd, arg);
}

int process_pipe(int *fds)
{
    struct thread *me = current;
    struct pipe *p;
    int r, w;

    if (!me || !fds)
        return -1;

    p = pipe_create();
    if (!p)
        return -1;

    group_lock(me);
    r = fd_alloc(me);
    if (r < 0) {
        group_unlock(me);
        kfree(p);
        return -1;
    }
    PFD(me)[r].type = FD_PIPE_R;    /* claim the slot before the next scan */
    PFD(me)[r].u.pipe = p;
    pipe_ref_reader(p);

    w = fd_alloc(me);
    if (w < 0) {
        PFD(me)[r].type = FD_NONE;
        group_unlock(me);
        kfree(p);
        return -1;
    }
    PFD(me)[w].type = FD_PIPE_W;
    PFD(me)[w].u.pipe = p;
    pipe_ref_writer(p);
    group_unlock(me);

    fds[0] = r;
    fds[1] = w;
    return 0;
}

int process_dup2(int oldfd, int newfd)
{
    struct thread *me = current;
    file_t *o, *n, tmp;

    if (!me)
        return -1;

    group_lock(me);
    o = fd_slot(me, oldfd);
    n = fd_slot(me, newfd);
    if (!o || !n || o->type == FD_NONE) {
        group_unlock(me);
        return -1;
    }
    if (oldfd == newfd) {
        group_unlock(me);
        return newfd;
    }

    /* Build the copy first: if it fails (no descriptors, no heap) the target
     * must be left alone rather than closed for nothing.  What "copy" means
     * per type is fd_clone()'s business - the same rules a child inherits
     * by. */
    if (fd_clone(&tmp, o) < 0) {
        group_unlock(me);
        return -1;
    }

    fd_close_in(me, newfd);
    *n = tmp;
    group_unlock(me);
    return newfd;
}

/* Duplicate onto the lowest free descriptor.  dup2() with the target chosen
 * from the table, so every type costs exactly what it costs there. */
int process_dup(int fd)
{
    struct thread *me = current;
    file_t *src;
    int nfd;

    if (!me)
        return -1;
    group_lock(me);
    src = fd_slot(me, fd);
    if (!src || src->type == FD_NONE) {
        group_unlock(me);
        return -1;
    }
    nfd = fd_alloc(me);
    if (nfd < 0) {
        group_unlock(me);
        return -1;
    }
    /* fd_clone() zeroes the slot before it fills it and leaves it empty when
     * it fails, so a failure needs no cleanup here. */
    if (fd_clone(&PFD(me)[nfd], src) < 0) {
        group_unlock(me);
        return -1;
    }
    group_unlock(me);
    return nfd;
}

/* Move a regular file's position.  The position lives in the descriptor and
 * the littlefs handle has to follow it, because read() advances the handle
 * itself.  A pipe or a device has no position, and a seek past either end of
 * the file is refused: this is the drive-a-file subset, not a way to grow
 * one. */
int process_lseek(int fd, int off, int whence)
{
    struct thread *me = current;
    file_t *f = fd_slot(me, fd);
    int32_t base, pos;

    if (!f || f->type != FD_FILE)
        return -1;

    switch (whence) {
    case 0:  base = 0;                 break;   /* SEEK_SET */
    case 1:  base = (int32_t)f->pos;   break;   /* SEEK_CUR */
    case 2:  base = (int32_t)f->size;  break;   /* SEEK_END */
    default:
        return -1;
    }
    pos = base + off;
    if (pos < 0 || (uint32_t)pos > f->size)
        return -1;
    if (ramdisk_seek(f->fh, (uint32_t)pos) < 0)
        return -1;
    f->pos = (uint32_t)pos;
    return pos;
}

/* Describe an open descriptor the way path_stat() describes its path, so a
 * program that has the fd does not need to remember the name it opened. */
int process_fstat(int fd, uint32_t *size, uint8_t *type)
{
    struct thread *me = current;
    file_t *f = fd_slot(me, fd);

    if (!f || f->type == FD_NONE)
        return -1;
    if (f->type == FD_FILE) {
        *size = f->size;
        *type = TOS_FTYPE_REG;
    } else {
        /* Console, pipe, /dev or /proc: a character stream, which is what
         * stat() says about the same node too. */
        *size = 0;
        *type = TOS_FTYPE_CHR;
    }
    return 0;
}

/* Readiness for poll().  Only the console can be "not yet": /proc and /dev
 * produce their text on demand, a file has a position and an EOF, and a pipe
 * has a count.  A regular file reports both directions because read() at EOF
 * returns 0 at once and write() either takes the bytes or fails at once. */
short process_poll(int fd, short events)
{
    struct thread *me = current;
    file_t *f = fd_slot(me, fd);
    short rev = 0;

    if (!f || f->type == FD_NONE)
        return TOS_POLLNVAL;

    switch (f->type) {
    case FD_CONSOLE:
        if (fd == 0) {
            if (tty_is_raw() ? tty_raw_ready() : tty_line_ready())
                rev |= TOS_POLLIN;
        } else {
            rev |= TOS_POLLOUT;
        }
        break;

    case FD_FILE:
        rev |= TOS_POLLIN | TOS_POLLOUT;
        break;

    case FD_CHRDEV:
        rev |= TOS_POLLIN | TOS_POLLOUT;
        break;

    case FD_PIPE_R:
        if (process_pipe_ready(f->u.pipe, 0))
            rev |= TOS_POLLIN;
        break;

    case FD_PIPE_W:
        if (process_pipe_ready(f->u.pipe, 1))
            rev |= TOS_POLLOUT;
        break;

    default:
        break;
    }

    return rev & (events | TOS_POLLERR | TOS_POLLNVAL);
}

