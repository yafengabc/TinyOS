#include "lib.h"

/* Burns CPU without ever giving it up: no sleep, no yield, only uptime() to
 * pace the output.  Started in the background it is the preemption demo -
 * the shell keeps answering while this runs, and without preemption it
 * would freeze until the loop is over. */
int main(void)
{
    int pid = getpid();
    int next = uptime();

    for (int i = 1; i <= 6; i++) {
        next += 400;
        while (uptime() < next)
            ;
        printf("spin[%d] %d/6 at %d ms\n", pid, i, uptime());
    }
    printf("spin[%d] done\n", pid);
    return 0;
}
