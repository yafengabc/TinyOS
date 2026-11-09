#include "lib.h"

/* wificonn <ssid> <password>: hand the credentials to the WiFi control
 * task and ask it to associate.  The association itself runs in the
 * driver's ktask; this command returns immediately. */
int main(int argc, char *argv[])
{
    char buf[128];
    int n = 0;
    int fd;

    if (argc < 3) {
        puts("usage: wificonn <ssid> <password>\n");
        return 1;
    }
    fd = open("/dev/wifi");
    if (fd < 0) {
        puts("wificonn: /dev/wifi not ready\n");
        return 1;
    }

    for (const char *p = argv[1]; *p && n < (int)sizeof(buf) - 3; p++)
        buf[n++] = *p;
    buf[n++] = ' ';
    for (const char *p = argv[2]; *p && n < (int)sizeof(buf) - 2; p++)
        buf[n++] = *p;
    buf[n++] = '\n';

    write(fd, buf, n);
    if (ioctl(fd, 1, 0) != 0) {
        puts("wificonn: request failed\n");
        close(fd);
        return 1;
    }
    close(fd);
    puts("connect requested\n");
    return 0;
}
