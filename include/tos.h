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
#define SYS_STAT     18  /* stat(path, &st) -> 0 or -1          */
#define SYS_MEMINFO  19  /* meminfo(&m) -> 0, fills struct       */
#define SYS_DF       20  /* dfinfo(&d) -> 0, fills struct        */
#define SYS_CHDIR    21  /* chdir(path) -> 0 or -1               */
#define SYS_GETCWD   22  /* getcwd(buf, len) -> bytes or -1      */
#define SYS_TTYRAW   23  /* ttyraw(on) -> previous mode, 0/1     */
#define SYS_CLOCK    24  /* clockinfo(&c) -> 0, fills struct     */
#define SYS_GETCPU   25  /* getcpu() -> id of the CPU running us */
#define SYS_GPIO     26  /* gpio(op, pin, arg) -> depends on op  */
#define SYS_ADC      27  /* adc(op, chan) -> depends on op       */
#define SYS_PIPE     28  /* pipe(int fds[2]) -> 0 or -1          */
#define SYS_DUP2     29  /* dup2(old, new) -> new or -1          */
#define SYS_IOCTL    30  /* ioctl(fd, cmd, arg) -> depends on cmd */
#define SYS_UNLINK   31  /* unlink(path) -> 0 or -1             */

/* Signals.  There are no handlers: a signal either kills the task or parks
 * it, both decided by the kernel. */
#define SIGKILL 9       /* terminate now                        */
#define SIGSTOP 19      /* stop until SIGCONT                   */
#define SIGCONT 18      /* resume a stopped task                */

/* File types returned by stat() (struct stat lives in user/lib.h).  These are
 * ABI twice over: `ls` picks its letter from them, and SYS_CHDIR insists on
 * TOS_FTYPE_DIR.  A character device is its own type rather than a regular
 * file of size 0, because "open it and read it" and "read this file" are
 * different promises. */
#define TOS_FTYPE_REG 0   /* regular file                        */
#define TOS_FTYPE_DIR 1   /* directory                           */
#define TOS_FTYPE_CHR 2   /* character device (/dev, /proc)      */

/* Pin modes for gpio_mode(), and the op codes the two multiplexed
 * peripheral syscalls take.  These are ABI: the kernel's periph.h maps them
 * onto the board's plat_gpio_* / plat_adc_* hooks. */
#define TOS_PIN_IN           0
#define TOS_PIN_OUT          1
#define TOS_PIN_IN_PULLUP    2
#define TOS_PIN_IN_PULLDOWN  3

#define GPIO_OP_COUNT  0   /* ()          -> number of pins              */
#define GPIO_OP_MODE   1   /* (pin, mode) -> 0 or -1                     */
#define GPIO_OP_WRITE  2   /* (pin, 0|1)  -> 0 or -1                     */
#define GPIO_OP_READ   3   /* (pin)       -> 0/1, -1 if not readable     */

#define ADC_OP_COUNT   0   /* ()     -> number of channels               */
#define ADC_OP_READ    1   /* (chan) -> raw counts, -1 on failure        */
#define ADC_OP_VREF    2   /* ()     -> reference in millivolts          */
#define ADC_OP_BITS    3   /* ()     -> resolution in bits               */

#endif
