#include "lib.h"

/* net: print the station's current IPv4 configuration. */
int main(void)
{
    char buf[192];
    int fd = open("/dev/net");
    int n;

    if (fd < 0) {
        puts("net: /dev/net not ready\n");
        return 1;
    }
    if ((n = read(fd, buf, sizeof(buf) - 1)) > 0) {
        buf[n] = 0;
        write_all(1, buf, n);
    } else {
        puts("net: no address yet\n");
    }
    close(fd);
    return 0;
}
