#include "lib.h"

/* Hold a task window without using the CPU: sleep in a loop, print nothing.
 *
 * Exists to make window accounting testable from the shell - `hold 5 &`
 * saturates the user area with idle tasks, so the next exec() has to say
 * "largest free block is N KiB" instead of quietly failing, and killing the
 * shell's foreground job must return every window.  spin (from the same
 * directory) is the opposite case: it keeps the CPU and exits on its own. */

int main(int argc, char **argv)
{
    int secs = (argc > 1) ? atoi(argv[1]) : 1;

    if (secs < 0)
        secs = 0;
    for (int i = 0; i < secs; i++)
        sleep(1000);
    return 0;
}
