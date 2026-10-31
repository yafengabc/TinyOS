#include "lib.h"

int main(void)
{
    puts("about to write to an unmapped address...\n");
    volatile unsigned int *p = (volatile unsigned int *)0xDEADB000u;
    *p = 0x12345678;
    puts("this line should never be printed\n");
    return 0;
}
