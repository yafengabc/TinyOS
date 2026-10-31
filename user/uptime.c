#include "lib.h"

int main(void)
{
    int ms = uptime();
    int frac = ms % 1000;
    puts("uptime: ");
    putint(ms);
    puts(" ms (");
    putint(ms / 1000);
    putchar('.');
    if (frac < 100) putchar('0');
    if (frac < 10) putchar('0');
    putint(frac);
    puts(" s), pid=");
    putint(getpid());
    puts("\n");
    return 0;
}
