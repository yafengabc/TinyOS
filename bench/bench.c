/* bench.c - one benchmark, three engines: pvm (csa), rvm (csc), rcc (native).
 *
 * The same four kernels are also written in Python for MicroPython
 * (bench/bench.py and bench_native.py).  Keep the kernels integer-only and
 * identical across languages: TinyOS `float` is Q16.16 while MicroPython's is
 * IEEE double, so a float kernel would compare two different machines.
 *
 * Every kernel prints two lines:
 *     BENCH <name> ms=<milliseconds>
 *     BENCH <name> sum=<checksum>
 * `sum` is the run's own checksum: if it differs across engines, the
 * comparison is meaningless and the row should be ignored.
 *
 * csc takes one conversion per printf, so `ms` and `sum` are separate calls;
 * it also has no `&`, so the loop accumulator wraps itself instead of masking.
 *
 * Sizes are chosen so the fastest engine (rcc on a 160 MHz C3) still runs for
 * tens of milliseconds; otherwise the 1 ms guest clock cannot resolve it.
 *
 * Build:
 *     csa bench.c bench.amx     -> pvm bench.amx
 *     csc bench.c bench.rir     -> rvm bench.rir
 *     rcc bench.rir bench.tos   -> benchc
 */

int fib(int n) {
    if (n < 2)
        return n;
    return fib(n - 1) + fib(n - 2);
}

int main() {
    int t0;
    int t1;
    int i;
    int j;
    int s;
    int a[256];

    /* 1. recursion: 46368 at the bottom of the tree */
    t0 = uptime();
    s = fib(24);
    t1 = uptime();
    printf("BENCH fib ms=%d\n", t1 - t0);
    printf("BENCH fib sum=%d\n", s);

    /* 2. integer loop: add + branch, accumulator wraps to stay inside int32 */
    t0 = uptime();
    s = 0;
    for (i = 0; i < 300000; i = i + 1) {
        s = s + i;
        if (s >= 1000000000)
            s = s - 1000000000;
    }
    t1 = uptime();
    printf("BENCH loop ms=%d\n", t1 - t0);
    printf("BENCH loop sum=%d\n", s);

    /* 3. memory: 1000 passes over a 256-word array */
    t0 = uptime();
    s = 0;
    for (j = 0; j < 1000; j = j + 1) {
        for (i = 0; i < 256; i = i + 1)
            a[i] = i * 3 + j;
        for (i = 0; i < 256; i = i + 1)
            s = s + a[i];
    }
    t1 = uptime();
    printf("BENCH array ms=%d\n", t1 - t0);
    printf("BENCH array sum=%d\n", s);

    /* 4. division: one remainder per iteration */
    t0 = uptime();
    s = 0;
    for (i = 0; i < 200000; i = i + 1)
        s = s + i % 7;
    t1 = uptime();
    printf("BENCH div ms=%d\n", t1 - t0);
    printf("BENCH div sum=%d\n", s);

    return 0;
}
