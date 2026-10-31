#ifndef TINYOS_PROCESS_H
#define TINYOS_PROCESS_H

#include "types.h"
#include "platform.h"   /* regs_t */

/* Processes and the round-robin scheduler.
 *
 * There is one kernel stack.  Traps from user mode always enter on its top,
 * so nothing of a kernel call survives between traps - which is exactly what
 * makes switching cheap: a task's whole state is the register frame in
 * proc_t.saved, and "switching" is returning a different frame pointer to the
 * trap epilogue.  Blocking is the same trick: the syscall saves its request
 * in the task and hands the CPU to somebody else; whoever completes the
 * request writes the result into the saved frame, so the task resumes
 * directly in user mode. */

void     process_init(void);
void     process_start_shell(const char *path) __attribute__((noreturn));

/* Trap entry points.  Both return the frame to resume: usually the one that
 * came in, but a switch (or a blocked syscall) hands back another task's. */
regs_t  *syscall_dispatch(regs_t *r);
regs_t  *process_exit(regs_t *r, int code);

/* Called by the arch on every timer tick, with the interrupted frame. */
void     process_tick(regs_t *r);

/* Trap exit: gives the CPU away if the slice is over. */
regs_t  *process_resched(regs_t *r);

/* Syscalls that may give up the CPU. */
regs_t  *process_wait(regs_t *r, int pid);
regs_t  *process_sleep(regs_t *r, uint32_t ms);
regs_t  *process_yield(regs_t *r);
regs_t  *process_block_read(regs_t *r, uint32_t ubuf, uint32_t len);

/* Spawn a child and return its pid; the caller keeps running and uses
 * wait() to collect the exit code. */
int      process_exec(regs_t *r, const char *path, char *const argv[], int argc);

int      process_current_pid(void);
uint32_t process_sbrk(uint32_t bytes);
int      process_dump(void);

/* Is [addr, addr+len) inside the current task's window?  Replaces the old
 * page-table probe: the flat model only has range checks. */
int      process_user_ok(uint32_t addr, uint32_t len);

/* file descriptor helpers (fds are per process, 0/1 are console) */
int  process_fd_open(const char *path);
int  process_fd_create(const char *path);
int  process_fd_read(int fd, void *buf, uint32_t len);
int  process_fd_write(int fd, const void *buf, uint32_t len);
void process_fd_close(int fd);

#endif
