#include "lib.h"

/* C7 shape check: the kernel runs send/recv/link/IRQ-pending over its
 * loopback lower.  The command exists so QEMU and the boards can prove the
 * endpoint contract before a real LP/C6 lower exists. */
int main(void)
{
    int r = remote_selftest();

    if (r == 0) {
        puts("rselftest: ok (loopback send/recv/link/poll)\n");
        return 0;
    }
    puts("rselftest: FAIL\n");
    return 1;
}
