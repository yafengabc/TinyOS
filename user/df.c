#include "lib.h"

/* `df` reports littlefs partition usage: total bytes, bytes consumed,
 * bytes available and the percentage used.  The numbers come from the
 * SYS_DF syscall, which asks the kernel's ramdisk layer (lfs_fs_size). */
int main(void)
{
    struct dfinfo d;
    if (dfinfo(&d) != 0) {
        puts("df: filesystem info unavailable\n");
        return 1;
    }

    uint32_t avail = (d.total > d.used) ? d.total - d.used : 0;
    unsigned int pct = d.total ? (d.used * 100u) / d.total : 0;

    puts("\n");
    puts("Filesystem             Total      Used     Avail      Use%\n");
    printf("littlefs           %9u %9u %9u %8u%%\n",
           d.total, d.used, avail, pct);
    return 0;
}
