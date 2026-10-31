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
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        write(1, buf, n);
    close(fd);
    return 0;
}
