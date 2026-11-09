#include "lib.h"

/* ping <a.b.c.d> [count]: queue echo requests in the network task.  The
 * replies are printed by the kernel task, so this command returns as soon
 * as the request is queued. */
int main(int argc, char *argv[])
{
    char buf[64];
    int n = 0;
    int fd;

    if (argc < 2) {
        puts("usage: ping <a.b.c.d> [count]\n");
        return 1;
    }
    fd = open("/dev/net");
    if (fd < 0) {
        puts("ping: /dev/net not ready\n");
        return 1;
    }
    for (const char *p = "ping "; *p; p++)
        buf[n++] = *p;
    for (const char *p = argv[1]; *p && n < (int)sizeof(buf) - 8; p++)
        buf[n++] = *p;
    if (argc > 2) {
        buf[n++] = ' ';
        for (const char *p = argv[2]; *p && n < (int)sizeof(buf) - 2; p++)
            buf[n++] = *p;
    }
    buf[n++] = '\n';
    write(fd, buf, n);
    close(fd);
    puts("ping requested\n");
    return 0;
}
