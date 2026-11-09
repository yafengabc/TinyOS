#include "lib.h"

/* host <name>: resolve an A record through /dev/net. */
int main(int argc, char *argv[])
{
    char buf[80];
    int n = 0;
    int fd;

    if (argc < 2) {
        puts("usage: host <name>\n");
        return 1;
    }
    fd = open("/dev/net");
    if (fd < 0) {
        puts("host: /dev/net not ready\n");
        return 1;
    }
    for (const char *p = "host "; *p; p++)
        buf[n++] = *p;
    for (const char *p = argv[1]; *p && n < (int)sizeof(buf) - 2; p++)
        buf[n++] = *p;
    buf[n++] = '\n';
    write(fd, buf, n);
    close(fd);
    puts("lookup requested\n");
    return 0;
}
