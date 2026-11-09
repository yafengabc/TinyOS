#include "lib.h"

/* http <host> [port] [path]: fetch a URL over a minimal TCP client. */
int main(int argc, char *argv[])
{
    char buf[160];
    int n = 0;
    int fd;

    if (argc < 2) {
        puts("usage: http <host> [port] [path]\n");
        return 1;
    }
    fd = open("/dev/net");
    if (fd < 0) {
        puts("http: /dev/net not ready\n");
        return 1;
    }
    for (const char *p = "http "; *p; p++)
        buf[n++] = *p;
    for (int a = 1; a < argc && n < (int)sizeof(buf) - 2; a++) {
        if (a > 1)
            buf[n++] = ' ';
        for (const char *p = argv[a]; *p && n < (int)sizeof(buf) - 2; p++)
            buf[n++] = *p;
    }
    buf[n++] = '\n';
    write(fd, buf, n);
    close(fd);
    puts("http requested\n");
    return 0;
}
