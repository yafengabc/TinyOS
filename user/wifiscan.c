#include "lib.h"

/* wifiscan: ask the WiFi control task to run one sweep.  The results are
 * printed from the driver's ktask (the scan itself must not run in a user
 * syscall context), so this command returns as soon as the request is
 * queued. */
int main(void)
{
    int fd = open("/dev/wifi");

    if (fd < 0) {
        puts("wifiscan: /dev/wifi not ready\n");
        return 1;
    }
    if (ioctl(fd, 0, 0) != 0) {
        puts("wifiscan: request failed\n");
        close(fd);
        return 1;
    }
    close(fd);
    puts("scan requested\n");
    return 0;
}
