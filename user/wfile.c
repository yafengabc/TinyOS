#include "lib.h"

/* Create a file and write the remaining arguments to it (space separated).
   Proves the ATA write path is reachable from user space. */

int main(int argc, char *argv[])
{
    if (argc < 2) {
        puts("usage: wfile <path> [text...]\n");
        return 1;
    }

    int fd = create(argv[1]);
    if (fd < 0) {
        puts("wfile: ");
        puts(argv[1]);
        puts(": cannot create\n");
        return 1;
    }

    for (int i = 2; i < argc; i++) {
        int n = (int)strlen(argv[i]);
        if (i > 2) {
            int r = write(fd, " ", 1);
            if (r < 0) goto fail;
        }
        int r = write(fd, argv[i], n);
        if (r != n) goto fail;
    }
    if (write(fd, "\n", 1) != 1)
        goto fail;
    close(fd);

    puts("wrote ");
    puts(argv[1]);
    puts("\n");
    return 0;

fail:
    puts("wfile: write error on ");
    puts(argv[1]);
    puts("\n");
    close(fd);
    return 1;}
