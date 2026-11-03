#ifndef TINYOS_PIPE_H
#define TINYOS_PIPE_H

#include "types.h"

/* Pipes - the kernel's inter-process channel.
 *
 * A pipe is a fixed ring buffer with two ends.  It knows nothing about the
 * scheduler: blocking, waking and copying to the right user buffer all
 * happen in process.c, which is the only place that may touch a task.  That
 * split is what keeps this file small enough to reason about, and it is the
 * same split the tty uses (kernel/tty.c holds bytes, process.c decides who
 * gets them).
 *
 * Lifetime is reference counted, but with two counters rather than one: a
 * reader must be able to see EOF the moment the last writer closes, without
 * waiting for the buffer to drain, and a writer must see EPIPE the moment
 * the last reader goes away.
 *
 * Concurrency: every entry point takes the pipe's own lock when the kernel
 * is built for more than one CPU (spin_t collapses to nothing otherwise), so
 * a reader on one core and a writer on the other cannot corrupt the ring. */

#define PIPE_SIZE 2048          /* bytes per pipe (heap allocated) */
#define PIPE_EPIPE (-2)         /* no readers left                  */
#define PIPE_EAGAIN (-1)        /* would block (or nothing to read)  */

struct pipe;

struct pipe *pipe_create(void);
void         pipe_ref_reader(struct pipe *p);
void         pipe_ref_writer(struct pipe *p);
/* Drop one end.  Returns 1 when that was the last reference of any kind, so
 * the caller knows it may free the pipe (after waking whoever needs to know
 * the other end closed). */
int          pipe_drop_reader(struct pipe *p);
int          pipe_drop_writer(struct pipe *p);

uint32_t     pipe_readers(struct pipe *p);
uint32_t     pipe_writers(struct pipe *p);

/* Read up to len bytes: >0 bytes, 0 for EOF (empty and no writers),
 * PIPE_EAGAIN when empty but writers still exist.  Never blocks. */
int          pipe_read(struct pipe *p, void *buf, uint32_t len);

/* Write up to len bytes: >0 bytes accepted, PIPE_EAGAIN when full,
 * PIPE_EPIPE when nobody is left to read.  Never blocks, and a short write
 * is normal - a pipe has no message boundaries. */
int          pipe_write(struct pipe *p, const void *buf, uint32_t len);

int          pipe_readable(struct pipe *p);
int          pipe_writable(struct pipe *p);

#endif
