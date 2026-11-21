/* benchclang - bench/bench.c built by the normal clang user-program path.
 *
 * Same four kernels and the same "BENCH <name> ms=" / "BENCH <name> sum="
 * lines as bench/bench.c, bench/bench.py and the rcc image, so the numbers go
 * straight into docs/BENCH.md.
 *
 * This directory is for measurement only: it is not part of a normal image
 * (see docs/BENCH.md for how it is built and removed).
 */
#include "lib.h"

TOS_STACK(4 * 1024);

static int fib(int n)
{
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

int main(void)
{
    int t0, t1, i, j, s;
    static int a[256];

    t0 = uptime();
    s = fib(24);
    t1 = uptime();
    printf("BENCH fib ms=%d\n", t1 - t0);
    printf("BENCH fib sum=%d\n", s);

    t0 = uptime();
    s = 0;
    for (i = 0; i < 300000; i++) {
        s = s + i;
        if (s >= 1000000000)
            s = s - 1000000000;
    }
    t1 = uptime();
    printf("BENCH loop ms=%d\n", t1 - t0);
    printf("BENCH loop sum=%d\n", s);

    t0 = uptime();
    s = 0;
    for (j = 0; j < 1000; j++) {
        for (i = 0; i < 256; i++)
            a[i] = i * 3 + j;
        for (i = 0; i < 256; i++)
            s = s + a[i];
    }
    t1 = uptime();
    printf("BENCH array ms=%d\n", t1 - t0);
    printf("BENCH array sum=%d\n", s);

    t0 = uptime();
    s = 0;
    for (i = 0; i < 200000; i++)
        s = s + i % 7;
    t1 = uptime();
    printf("BENCH div ms=%d\n", t1 - t0);
    printf("BENCH div sum=%d\n", s);

    return 0;
}
