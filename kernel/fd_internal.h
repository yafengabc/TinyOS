#ifndef TINYOS_FD_INTERNAL_H
#define TINYOS_FD_INTERNAL_H

#include "file.h"

/* The seam between the descriptor table (fd.c) and the scheduler half
 * (process.c).  fd.c owns what a descriptor means; process.c owns the table
 * storage (struct proc) and the tasks that block on pipes, so the six hooks
 * below are implemented there and called here.
 *
 * Callers hold the group lock (group_lock()/proc_fd_lock()) except for
 * process_pipe_closed()/process_pipe_ready(), which take proc_lock
 * themselves.
 */

struct thread;

/* struct proc's table and its lock, reached through the thread. */
file_t *proc_fd_table(struct thread *t);
void    proc_fd_lock(struct thread *t);
void    proc_fd_unlock(struct thread *t);

/* A pipe end was closed: wake the other side, free the pipe if that was the
 * last reference.  write_end says which end went away. */
void    process_pipe_closed(struct pipe *p, int write_end);

/* Readiness of one end of a pipe for poll(). */
int     process_pipe_ready(struct pipe *p, int write_end);

/* Shared between the two halves: read()/write() in process.c need the slot,
 * exec() needs fd_clone(), task_release() and close() need fd_close_in(). */
file_t *fd_slot(struct thread *p, int fd);
int     fd_clone(file_t *dst, const file_t *src);
void    fd_close_in(struct thread *p, int fd);

#endif /* TINYOS_FD_INTERNAL_H */
