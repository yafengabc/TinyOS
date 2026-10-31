#include "lib.h"
#include "tos.h"

static inline int sys0(int n)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n) : "memory");
    return r;
}

static inline int sys1(int n, int a)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}

static inline int sys2(int n, int a, int b)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b) : "memory");
    return r;
}

static inline int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

int write(int fd, const void *buf, int len)
{
    return sys3(SYS_WRITE, fd, (int)buf, len);
}

int read(int fd, void *buf, int len)
{
    return sys3(SYS_READ, fd, (int)buf, len);
}

int exec(const char *path, char *const argv[], int argc)
{
    return sys3(SYS_EXEC, (int)path, (int)argv, argc);
}

int open(const char *path)
{
    return sys1(SYS_OPEN, (int)path);
}

int create(const char *path)
{
    return sys1(SYS_CREATE, (int)path);
}

int close(int fd)
{
    return sys1(SYS_CLOSE, fd);
}

int readdir(int index, char *name, int len)
{
    return sys3(SYS_READDIR, index, (int)name, len);
}

int getpid(void)
{
    return sys0(SYS_GETPID);
}

int sleep(int ms)
{
    return sys1(SYS_SLEEP, ms);
}

int uptime(void)
{
    return sys0(SYS_UPTIME);
}

void *sbrk(int bytes)
{
    int r = sys1(SYS_SBRK, bytes);
    return (void *)r;
}

void exit(int code)
{
    sys1(SYS_EXIT, code);
    for (;;)
        ;
}
