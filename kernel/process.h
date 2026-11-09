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

/* Hand queued console input to a blocked reader.  The tick calls it once
 * per tick; a board that receives console input by interrupt calls it
 * right after the drain so the reader wakes immediately (see the C3's
 * UART0 / USB-Serial/JTAG lines).  Trap context, no frame in, no frame
 * out: the woken task simply joins the run queue. */
void     process_console_kick(void);

/* Trap exit: gives the CPU away if the slice is over. */
regs_t  *process_resched(regs_t *r);

/* The saved frame of a task that has not run yet.  proc_t is private to
 * process.c, so a secondary CPU starting its first task (kernel/cpu.c)
 * cannot reach the member itself; this is the one accessor it needs. */
struct proc;
regs_t  *process_saved_frame(struct proc *p);

/* The frame of the task this CPU is running.  Used by ports that have to
 * rewrite part of a frame when a task moves to another core - on ARM the
 * kernel stack pointer stored in the frame belongs to whichever core built
 * it, so it has to be re-based at every switch.  NULL before the first task
 * exists. */
regs_t  *process_current_frame(void);

/* Kernel threads used by the WiFi OSI layer.  They run in M-mode on their
 * own stack; start.S reads ktask_running because mret clears MPP. */
extern volatile int ktask_running;
int      process_ktask_create(const char *name, void (*fn)(void *), void *arg,
                              uint32_t stack_bytes, int prio);
/* Opaque handle for the OSI layer (the current proc_t). */
void    *process_current_task(void);
void     process_ktask_sleep(uint32_t ticks);
void     process_ktask_yield(void);
regs_t  *process_ktask_syscall(regs_t *r);

/* Syscalls that may give up the CPU. */
regs_t  *process_wait(regs_t *r, int pid);
regs_t  *process_sleep(regs_t *r, uint32_t ms);
regs_t  *process_yield(regs_t *r);
regs_t  *process_block_read(regs_t *r, uint32_t ubuf, uint32_t len);

/* Spawn a child and return its pid; the caller keeps running and uses
 * wait() to collect the exit code. */
int      process_exec(regs_t *r, const char *path, char *const argv[], int argc);

int      process_current_pid(void);
/* Move a task to another priority level (0 = highest, 2 = lowest).  A task
 * cannot climb above the caller's own level, except that the first task
 * (the shell) may place a child anywhere - that is how a 1 ms job is given
 * the CPU ahead of everything else.  pid 0 means the caller. */
int      process_setprio(int pid, int prio);
uint32_t process_sbrk(uint32_t bytes);
int      process_dump(void);

/* Send `sig` to task `pid`.  SIGKILL terminates it, SIGSTOP parks it until
 * SIGCONT.  Returns 0, or -1 when there is no such task, the signal is not
 * one the kernel knows, or the task cannot act on it in its current state.
 * Console ^C / ^Z / ^\ arrive here too: the tty layer cannot touch another
 * task, only the kernel can. */
int      process_signal(int pid, int sig);

/* Called on the way out of a trap: parks the current task if SIGSTOP
 * arrived while it was running.  Every port's trap exit must call it. */
regs_t  *process_suspend(regs_t *r);

/* pid of the task blocked reading the console, or 0 when none is.  That is
 * the foreground task as far as the terminal is concerned. */
int      process_console_reader(void);

/* Who ^C / ^Z act on: the console reader, else the job a blocked parent
 * waits for, else the task that outranks the shell.  0 when nothing does. */
int      process_ctrl_target(void);

/* Is [addr, addr+len) inside the current task's window?  Replaces the old
 * page-table probe: the flat model only has range checks. */
int      process_user_ok(uint32_t addr, uint32_t len);

/* Descriptor operations.  read/write can block (console, full or empty
 * pipe), so they return the frame to resume - the caller's own when the
 * operation completed, the next task's when it blocked. */
regs_t  *process_read(regs_t *r, int fd, uint32_t ubuf, uint32_t len);
regs_t  *process_write(regs_t *r, int fd, uint32_t ubuf, uint32_t len);

int  process_fd_open(const char *path);
int  process_fd_create(const char *path);
int  process_fd_close(int fd);

/* Create a pipe, install both ends in the caller and write the two
 * descriptor numbers into `fds` (user memory). */
int  process_pipe(int *fds);

/* Duplicate a descriptor onto another number, closing the target first.
 * Every type is duplicable: a file is reopened by its saved path, a device
 * gets its own per-open state, a pipe shares its end by refcount. */
int  process_dup2(int oldfd, int newfd);

/* Configuration on an open character device (pin direction, ADC metadata).
 * -1 for anything that is not a device, or a device without an ioctl. */
int  process_ioctl(int fd, int cmd, int arg);

/* Format the process table into a buffer, for /proc/ps and for `ps`.
 * Returns the number of bytes written; output is truncated, not overrun. */
uint32_t process_ps_text(char *buf, uint32_t cap);

/* Deepest stack any task has ever used, in bytes, and (in `name`, copied and
 * NUL terminated) which task reached it.  The record survives the task, so a
 * program that has already exited still answers "was 16 KiB of stack the
 * right number?" - see the stk column of ps for the live value. */
uint32_t process_stack_peak(char *name, uint32_t cap);

/* Called by a port's user-fault path when a load or store ran off the bottom
 * of the stack (`addr` is the address it wanted, `sp` the user stack pointer
 * at the time).  If the task's window still has heap slack, the stack is
 * doubled into it and the protection is reprogrammed; the caller then retries
 * the instruction, which faulted before writing anything.  Returns 1 when the
 * fault was handled, 0 when it is a real fault and the task must die. */
int process_stack_grow(uint32_t addr, uint32_t sp);

/* Current working directory of the calling task.  chdir()/getcwd() and the
 * directory-relative open/stat/readdir syscalls ride on this. */
const char *process_cwd_str(void);
void        process_set_cwd_str(const char *path);

#endif
