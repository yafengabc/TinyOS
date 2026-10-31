#ifndef TINYOS_PROCESS_H
#define TINYOS_PROCESS_H

#include "types.h"
#include "platform.h"   /* regs_t */

void     process_init(void);
void     process_start_shell(const char *path) __attribute__((noreturn));
int      process_exec(regs_t *r, const char *path, char *const argv[], int argc);
void     process_kill_current(regs_t *r, int code);
int      process_current_pid(void);
uint32_t process_current_cr3(void);
uint32_t process_sbrk(uint32_t bytes);

/* file descriptor helpers (fds are per process, 0/1 are console) */
int  process_fd_open(const char *path);
int  process_fd_create(const char *path);
int  process_fd_read(int fd, void *buf, uint32_t len);
int  process_fd_write(int fd, const void *buf, uint32_t len);
void process_fd_close(int fd);

#endif
