# bench.py - the four kernels from bench/bench.c in MicroPython.
#
#   mpremote run bench/bench.py            plain bytecode
#   mpremote run bench/bench_native.py     @micropython.native
#
# Integer-only on purpose: TinyOS float is Q16.16, MicroPython's is IEEE
# double, so a float kernel would not be the same machine.  Each kernel prints
# the same two lines as the C version ("BENCH <name> ms=<n>" then
# "BENCH <name> sum=<n>"); the checksums must match across every engine or the
# run is not comparable.

import time


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def main():
    a = [0] * 256

    t0 = time.ticks_ms()
    s = fib(24)
    t1 = time.ticks_ms()
    print("BENCH fib ms=%d" % (t1 - t0))
    print("BENCH fib sum=%d" % (s,))

    t0 = time.ticks_ms()
    s = 0
    for i in range(300000):
        s = s + i
        if s >= 1000000000:
            s = s - 1000000000
    t1 = time.ticks_ms()
    print("BENCH loop ms=%d" % (t1 - t0))
    print("BENCH loop sum=%d" % (s,))

    t0 = time.ticks_ms()
    s = 0
    for j in range(1000):
        for i in range(256):
            a[i] = i * 3 + j
        for i in range(256):
            s = s + a[i]
    t1 = time.ticks_ms()
    print("BENCH array ms=%d" % (t1 - t0))
    print("BENCH array sum=%d" % (s,))

    t0 = time.ticks_ms()
    s = 0
    for i in range(200000):
        s = s + i % 7
    t1 = time.ticks_ms()
    print("BENCH div ms=%d" % (t1 - t0))
    print("BENCH div sum=%d" % (s,))


main()
