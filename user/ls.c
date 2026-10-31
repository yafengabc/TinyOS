#include "lib.h"

int main(void)
{
    char name[64];
    for (int i = 0;; i++) {
        int n = readdir(i, name, sizeof(name));
        if (n <= 0)
            break;
        puts(name);
        putchar('\n');
    }
    return 0;
}
