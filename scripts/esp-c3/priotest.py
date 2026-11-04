#!/usr/bin/env python3
"""Sampling cadence at NORMAL vs the real-time level, on real hardware.

`nice 0` only means something if level 0 is free - the first task used to be
started there, which made every task a real-time task and the knob inert.
This measures the difference the way the use case does: user/samp.c sleeps
1 ms a thousand times and reports how far apart the samples actually landed,
with N same-priority CPU-bound jobs (`run spin &`) next to it.

Each configuration starts from a reset, because the burners live 2.4 s and a
leftover one silently turns the next measurement into a different test.

    python3 scripts/esp-c3/priotest.py COM4
"""
import argparse
import re
import sys
import time

import serial

SAMP = re.compile(r"samp: <=1ms (\d+)\s+2ms (\d+)\s+3ms (\d+)\s+>3ms (\d+)\s+worst (\d+) ms")


def reset(ser):
    ser.dtr = False
    ser.rts = True
    time.sleep(0.15)
    ser.reset_input_buffer()
    ser.rts = False
    time.sleep(0.25)


def wait_prompt(ser, timeout=8.0):
    buf = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            if buf.count(b"tos> ") >= 1:
                return bytes(buf)
    return bytes(buf)


def command(ser, cmd, until, timeout):
    ser.reset_input_buffer()
    ser.write(cmd)
    buf = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            if until in buf:
                break
    return bytes(buf).decode("utf-8", "backslashreplace")


def cadence(ser, cmd, hogs):
    """Start `hogs` burners, run `cmd`, return (histogram, seconds)."""
    started = time.time()
    for _ in range(hogs):
        command(ser, b"run spin &\n", b"tos> ", 5)
    out = command(ser, cmd, b"samp:", 20)
    took = time.time() - started
    m = SAMP.search(out)
    if not m:
        raise SystemExit("no samp line in: %r" % out[-300:])
    return [int(g) for g in m.groups()], took


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--hogs", type=int, nargs="+", default=[0, 1, 2, 3])
    args = ap.parse_args()

    ser = serial.Serial(args.port, 115200, timeout=0.05)
    time.sleep(0.2)
    rows = []

    for k in args.hogs:
        for label, cmd in (("normal", b"samp\n"), ("nice 0", b"nice 0 samp\n")):
            reset(ser)
            wait_prompt(ser)
            hist, took = cadence(ser, cmd, k)
            rows.append((k, label, hist))
            print("%d hog  %-7s  <=1ms %4d  2ms %4d  3ms %4d  >3ms %3d   worst %d ms  (%.1fs)"
                  % (k, label, hist[0], hist[1], hist[2], hist[3], hist[4], took))
            sys.stdout.flush()

    print()
    print("worst gap by configuration (ms):")
    for k, label, hist in rows:
        print("  %d hog  %-7s  %d" % (k, label, hist[4]))

    # What "pass" can honestly mean.  A 1 ms loop on a 1 ms tick has no slack:
    # the sleep call computes its duration from a clock read a few
    # microseconds earlier, and a tick landing in that window costs one tick.
    # That floor is the same at every priority, so the test asks for (a) a
    # real-time sampler that never does worse than NORMAL, (b) a worst gap of
    # one tick, and (c) under real load, at least an order of magnitude fewer
    # misses than NORMAL.
    problems = []
    for k in args.hogs:
        normal = next(h for kk, lab, h in rows if kk == k and lab == "normal")
        rt = next(h for kk, lab, h in rows if kk == k and lab == "nice 0")
        normal_bad = 1000 - normal[0]
        rt_bad = 1000 - rt[0]
        if rt_bad > normal_bad:
            problems.append("%d hog: real-time (%d misses) is worse than normal (%d)"
                            % (k, rt_bad, normal_bad))
        if rt[4] > 2:
            problems.append("%d hog: real-time worst gap %d ms" % (k, rt[4]))
        if k >= 2 and rt_bad > 30:
            problems.append("%d hog: real-time missed %d of 1000" % (k, rt_bad))

    print()
    if problems:
        for p in problems:
            print("FAIL: " + p)
        return 1
    print("PASS: real-time sampling held one tick (worst 2 ms) and never lost to NORMAL")
    return 0


if __name__ == "__main__":
    sys.exit(main())
