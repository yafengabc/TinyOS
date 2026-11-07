#include "lib.h"

int main(int argc, char *argv[])
{
    if (argc < 2) {
        puts("usage: cat <file>\n");
        return 1;
    }

    int fd = open(argv[1]);
    if (fd < 0) {
        puts("cat: ");
        puts(argv[1]);
        puts(": no such file\n");
        return 1;
    }

    char buf[256];
    int n;
    /* write_all, not write: on the far end of a pipe a write comes back
       short as soon as the ring is full, and the old loop threw the rest of
       the block away (OPEN-ITEMS §24). */
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        if (write_all(1, buf, n) != n) {
            close(fd);
            return 1;
        }
    close(fd);
    return 0;
}
