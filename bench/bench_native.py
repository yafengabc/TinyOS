# bench_native.py - bench/bench.py with MicroPython's native emitter.
#
#   mpremote run bench/bench_native.py
#
# @micropython.native compiles the function to native code through the same
# emitter as the bytecode path, so it isolates "native codegen" from the
# interpreter without pulling in Viper's type annotations.  The kernels and
# the output format are identical to bench/bench.py.

import time


@micropython.native
def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


@micropython.native
def loop_kernel():
    s = 0
    for i in range(300000):
        s = s + i
        if s >= 1000000000:
            s = s - 1000000000
    return s


@micropython.native
def array_kernel(a):
    s = 0
    for j in range(1000):
        for i in range(256):
            a[i] = i * 3 + j
        for i in range(256):
            s = s + a[i]
    return s


@micropython.native
def div_kernel():
    s = 0
    for i in range(200000):
        s = s + i % 7
    return s


def main():
    a = [0] * 256

    t0 = time.ticks_ms()
    s = fib(24)
    t1 = time.ticks_ms()
    print("BENCH fib ms=%d" % (t1 - t0))
    print("BENCH fib sum=%d" % (s,))

    t0 = time.ticks_ms()
    s = loop_kernel()
    t1 = time.ticks_ms()
    print("BENCH loop ms=%d" % (t1 - t0))
    print("BENCH loop sum=%d" % (s,))

    t0 = time.ticks_ms()
    s = array_kernel(a)
    t1 = time.ticks_ms()
    print("BENCH array ms=%d" % (t1 - t0))
    print("BENCH array sum=%d" % (s,))

    t0 = time.ticks_ms()
    s = div_kernel()
    t1 = time.ticks_ms()
    print("BENCH div ms=%d" % (t1 - t0))
    print("BENCH div sum=%d" % (s,))


main()
