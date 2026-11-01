#ifndef TINYOS_TOSAPI_H
#define TINYOS_TOSAPI_H

/* System call numbers, shared between the kernel and user programs. */

#define SYS_EXIT     0   /* exit(code)                          */
#define SYS_WRITE    1   /* write(fd, buf, len)                 */
#define SYS_READ     2   /* read(fd, buf, len)                  */
#define SYS_EXEC     3   /* exec(path, argv, argc)              */
#define SYS_OPEN     4   /* open(path)                          */
#define SYS_CLOSE    5   /* close(fd)                           */
#define SYS_READDIR  6   /* readdir(index, namebuf, len)        */
#define SYS_GETPID   7   /* getpid()                            */
#define SYS_SLEEP    8   /* sleep(ms)                           */
#define SYS_UPTIME   9   /* uptime() -> ms                      */
#define SYS_SBRK     10  /* sbrk(bytes) -> old break or -1      */
#define SYS_CREATE   11  /* create(path) -> fd (truncates)      */
#define SYS_WAIT     12  /* wait(pid) -> exit code, blocks      */
#define SYS_YIELD    13  /* yield() -> 0, give up the CPU       */
#define SYS_PS       14  /* ps() -> task count, prints a table  */
#define SYS_SIGNAL   15  /* signal(pid, sig) -> 0 or -1         */
#define SYS_REBOOT   16  /* reboot() -> does not return         */
#define SYS_SETPRIO  17  /* setprio(pid, prio) -> 0 or -1       */

/* Signals.  There are no handlers: a signal either kills the task or parks
 * it, both decided by the kernel. */
#define SIGKILL 9       /* terminate now                        */
#define SIGSTOP 19      /* stop until SIGCONT                   */
#define SIGCONT 18      /* resume a stopped task                */

#endif
