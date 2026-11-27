#include "lib.h"

/* reboot - reset the machine.
 *
 * The one implementation: sh used to have a builtin as well, but that only
 * meant two messages ("rebooting..." vs this one) and a reset that could
 * not be backgrounded or redirected.  Now `reboot` is a program
 * everywhere.
 * Exits with 2 if the port could only halt - reboot() is noreturn by
 * contract, so that branch is unreachable but keeps the compiler honest. */
int main(void)
{
    puts("reboot: resetting the machine\n");
    reboot();
    return 2;
}
