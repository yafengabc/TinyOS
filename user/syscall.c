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

int readdir(int index, char *name, int len)
{
    return tos_sys3(SYS_READDIR, index, (int)name, len);
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
