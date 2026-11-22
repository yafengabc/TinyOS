/* Host harness for kernel/uaccess.c.
 *
 * The range check is the only thing between a corrupt image and the kernel on
 * the flat-memory ports, and every case here is a boundary the loader can be
 * handed by a file.  The window is [0x1000, 0x3000): four pages.
 */
#include <stdio.h>

#include "uaccess.h"

static int failures;

static void want(const char *what, int got, int expect)
{
    if (got != expect) {
        printf("FAIL %s: got %d want %d\n", what, got, expect);
        failures++;
    }
}

int main(void)
{
    const uint32_t lo = 0x1000, hi = 0x3000;

    want("len 0 below the window", uaccess_range_ok(lo, hi, 0, 0), 1);
    want("len 0 at hi",           uaccess_range_ok(lo, hi, hi, 0), 1);
    want("one byte below lo",     uaccess_range_ok(lo, hi, lo - 1, 1), 0);
    want("first byte",            uaccess_range_ok(lo, hi, lo, 1), 1);
    want("last byte",             uaccess_range_ok(lo, hi, hi - 1, 1), 1);
    want("one past hi",           uaccess_range_ok(lo, hi, hi, 1), 0);
    want("ends exactly at hi",    uaccess_range_ok(lo, hi, hi - 2, 2), 1);
    want("crosses hi",            uaccess_range_ok(lo, hi, hi - 1, 2), 0);
    want("whole window",          uaccess_range_ok(lo, hi, lo, hi - lo), 1);
    want("one byte too much",     uaccess_range_ok(lo, hi, lo, hi - lo + 1), 0);
    /* va + len wraps: the sum is small, the write is not. */
    want("va + len wraps",        uaccess_range_ok(lo, hi, 0xffffff00u, 0x200), 0);
    want("va near max, len 0",    uaccess_range_ok(lo, hi, 0xffffffffu, 0), 1);

    if (failures == 0) {
        printf("uaccess ok\n");
    }
    return failures ? 1 : 0;
}
