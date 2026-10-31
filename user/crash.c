#include "lib.h"

int main(void)
{
    puts("about to execute an illegal instruction...\n");
    /* The flat model has no page faults, so crash on something every
       architecture provides: an undefined instruction. */
    __asm__ volatile("ud2");
    puts("this line should never be printed\n");
    return 0;
}
