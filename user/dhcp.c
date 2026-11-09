#include "lib.h"

/* dhcp [renew]: drop the current lease and restart discovery, or force a
 * renewal of the current lease. */
int main(int argc, char *argv[])
{
    int fd = open("/dev/net");

    if (fd < 0) {
        puts("dhcp: /dev/net not ready\n");
        return 1;
    }
    if (argc > 1 && strcmp(argv[1], "renew") == 0)
        write(fd, "renew\n", 6);
    else
        write(fd, "dhcp\n", 5);
    close(fd);
    puts(argc > 1 ? "dhcp renew requested\n" : "dhcp restart requested\n");
    return 0;
}
