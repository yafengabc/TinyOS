#include "lib.h"
#include "tos.h"
#include "usys.h"     /* arch/<arch>/usys.h: the trap instruction + ABI */

/* Thin wrappers over tos_sys0..3, whose implementation differs per
 * architecture (int 0x80 on i386, ecall on RV32). */

int write(int fd, const void *buf, int len)
{
    return tos_sys3(SYS_WRITE, fd, (int)buf, len);
}

int read(int fd, void *buf, int len)
{
    return tos_sys3(SYS_READ, fd, (int)buf, len);
}

int exec(const char *path, char *const argv[], int argc)
{
    return tos_sys3(SYS_EXEC, (int)path, (int)argv, argc);
}

int open(const char *path)
{
    return tos_sys1(SYS_OPEN, (int)path);
}

int create(const char *path)
{
    return tos_sys1(SYS_CREATE, (int)path);
}

int close(int fd)
{
    return tos_sys1(SYS_CLOSE, fd);
}

int unlink(const char *path)
{
    return tos_sys1(SYS_UNLINK, (int)path);
}

int readdir(int index, char *name, int len)
{
    return tos_sys3(SYS_READDIR, index, (int)name, len);
}

int stat(const char *path, struct stat *st)
{
    return tos_sys2(SYS_STAT, (int)path, (int)st);
}

int meminfo(struct meminfo *m)
{
    return tos_sys1(SYS_MEMINFO, (int)m);
}

int dfinfo(struct dfinfo *d)
{
    return tos_sys1(SYS_DF, (int)d);
}

int chdir(const char *path)
{
    return tos_sys1(SYS_CHDIR, (int)path);
}

int getcwd(char *buf, int len)
{
    return tos_sys2(SYS_GETCWD, (int)buf, len);
}

int ttyraw(int on)
{
    return tos_sys1(SYS_TTYRAW, on);
}

int getpid(void)
{
    return tos_sys0(SYS_GETPID);
}

int sleep(int ms)
{
    return tos_sys1(SYS_SLEEP, ms);
}

int uptime(void)
{
    return tos_sys0(SYS_UPTIME);
}

int clockinfo(struct clockinfo *c)
{
    return tos_sys1(SYS_CLOCK, (int)c);
}

uint64_t clock_ns(void)
{
    struct clockinfo c;

    if (clockinfo(&c) < 0)
        return 0;
    return c.ns;
}

uint32_t clock_ms(void)
{
    struct clockinfo c;

    if (clockinfo(&c) < 0)
        return 0;
    return c.ms;
}

int getcpu(void)
{
    return tos_sys0(SYS_GETCPU);
}

/* ---- peripherals ------------------------------------------------------ */

int gpio_count(void)
{
    return tos_sys3(SYS_GPIO, GPIO_OP_COUNT, 0, 0);
}

int gpio_mode(int pin, int mode)
{
    return tos_sys3(SYS_GPIO, GPIO_OP_MODE, pin, mode);
}

int gpio_write(int pin, int value)
{
    return tos_sys3(SYS_GPIO, GPIO_OP_WRITE, pin, value);
}

int gpio_read(int pin)
{
    return tos_sys3(SYS_GPIO, GPIO_OP_READ, pin, 0);
}

int adc_count(void)
{
    return tos_sys2(SYS_ADC, ADC_OP_COUNT, 0);
}

int adc_read(int chan)
{
    return tos_sys2(SYS_ADC, ADC_OP_READ, chan);
}

int adc_vref_mv(void)
{
    return tos_sys2(SYS_ADC, ADC_OP_VREF, 0);
}

int adc_bits(void)
{
    return tos_sys2(SYS_ADC, ADC_OP_BITS, 0);
}

/* ---- pipes ------------------------------------------------------------ */

int pipe(int fds[2])
{
    return tos_sys1(SYS_PIPE, (int)fds);
}

int dup2(int oldfd, int newfd)
{
    return tos_sys2(SYS_DUP2, oldfd, newfd);
}

/* Configuration on an open device: pin direction, ADC reference voltage.
 * Text read/write covers the data plane (`cat /dev/adc2`, `echo 1 >
 * /dev/gpio4`); this is the half that has no sensible text form. */
int ioctl(int fd, int cmd, int arg)
{
    return tos_sys3(SYS_IOCTL, fd, cmd, arg);
}

int wait(int pid)
{
    return tos_sys1(SYS_WAIT, pid);
}

int yield(void)
{
    return tos_sys0(SYS_YIELD);
}

int ps(void)
{
    return tos_sys0(SYS_PS);
}

int setprio(int pid, int prio)
{
    return tos_sys2(SYS_SETPRIO, pid, prio);
}

void *sbrk(int bytes)
{
    int r = tos_sys1(SYS_SBRK, bytes);
    return (void *)r;
}

void exit(int code)
{
    tos_sys1(SYS_EXIT, code);
    for (;;)
        ;
}

/* The kernel resets the machine instead of returning.  If a port can only
 * halt, this is where the task stops for good. */
void reboot(void)
{
    tos_sys0(SYS_REBOOT);
    for (;;)
        ;
}
