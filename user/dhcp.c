#include "lib.h"

/* dhcp: drop the current lease and restart discovery. */
int main(void)
{
    int fd = open("/dev/net");

    if (fd < 0) {
        puts("dhcp: /dev/net not ready\n");
        return 1;
    }
    write(fd, "dhcp\n", 5);
    close(fd);
    puts("dhcp restart requested\n");
    return 0;
}
