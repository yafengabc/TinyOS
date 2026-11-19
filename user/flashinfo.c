#include "lib.h"

/* flashinfo - what the platform detected about the primary flash and how big
 * the filesystem ended up.  The detection (plat_flash_detected_bytes) is the
 * port's job: ROM FLASH_DEVINFO on RP2350, the boot loader's JEDEC record on
 * the C3, and a configured fallback where the port cannot tell. */
int main(void)
{
    int fd = open("/proc/flash");
    char buf[256];
    int n;

    if (fd < 0) {
        puts("flashinfo: /proc/flash unavailable\n");
        return 1;
    }
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        if (write_all(1, buf, n) != n) {
            close(fd);
            return 1;
        }
    close(fd);
    return 0;
}
