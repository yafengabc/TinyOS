#include "lib.h"

/* reboot - reset the machine.
 *
 * The shell has a `reboot` builtin too; this is the same call as a program,
 * so a script or another program can trigger a reset without the shell.
 * Exits with 2 if the port could only halt - reboot() is noreturn by
 * contract, so that branch is unreachable but keeps the compiler honest. */
int main(void)
{
    puts("reboot: resetting the machine\n");
    reboot();
    return 2;
}
