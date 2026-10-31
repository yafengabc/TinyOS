#include "lib.h"

int main(void)
{
    puts("heap demo, pid=");
    putint(getpid());
    puts("\n");

    char *p = malloc(200000);
    if (!p) {
        puts("malloc failed\n");
        return 1;
    }

    for (int i = 0; i < 200000; i += 4096)
        p[i] = (char)(i / 4096);

    int sum = 0;
    for (int i = 0; i < 200000; i += 4096)
        sum += p[i];

    puts("touched ");
    putint(200000 / 4096);
    puts(" pages, checksum=");
    putint(sum);
    puts("\n");
    free(p);
    puts("done\n");
    return 0;
}
