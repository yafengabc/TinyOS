#!/usr/bin/env python3
"""How long does the shell wait for the CPU?

`uptime` takes microseconds to run, so the time between sending the line and
the prompt coming back is almost entirely scheduling latency: how long the
shell sat READY behind somebody else.  That is the number a quantum is
supposed to bound - the tick rate bounds how often it is looked at, and a
blocking read bounds it only when the task is asleep.

Measured twice:

  idle   nobody else runnable; the shell is woken by the console interrupt
         and the idle loop's next tick
  load   a same-priority CPU burner (`run spin &`) is runnable, so the shell
         waits for that task's slice to expire

`run spin &` dies after ~2.4 s, so a batch is a handful of samples long and
the burner is restarted for the next batch.

    python3 scripts/esp-c3/schedlat.py COM4
"""
import argparse
import statistics
import sys
import time

import serial

PROMPT = b"tos> "


def open_port(port, reset):
    ser = serial.Serial(port, 115200, timeout=0.02)
    time.sleep(0.2)
    if reset:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.15)
        ser.reset_input_buffer()
        ser.rts = False
    return ser


def read_for(ser, seconds):
    buf = bytearray()
    end = time.time() + seconds
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            end = min(end, time.time() + seconds)
    return bytes(buf)


def wait_prompt(ser, timeout=6.0):
    buf = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        chunk = ser.read(4096)
        if not chunk:
            continue
        buf += chunk
        if PROMPT in buf:
            return True
    return False


def sample(ser):
    """Send `uptime`, return milliseconds until the prompt returns."""
    ser.reset_input_buffer()
    t0 = time.time()
    ser.write(b"uptime\n")
    buf = bytearray()
    while time.time() - t0 < 3.0:
        chunk = ser.read(4096)
        if not chunk:
            continue
        buf += chunk
        if PROMPT in buf:
            return (time.time() - t0) * 1000.0
    return None


def stats(name, xs):
    xs = [x for x in xs if x is not None]
    if not xs:
        print("%-6s no samples" % name)
        return
    print("%-6s n=%-3d median=%6.1f ms  p90=%6.1f  max=%6.1f  min=%5.1f"
          % (name, len(xs), statistics.median(xs),
             sorted(xs)[int(len(xs) * 0.9) - 1] if len(xs) > 1 else xs[0],
             max(xs), min(xs)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--batches", type=int, default=3)
    ap.add_argument("--per-batch", type=int, default=6)
    ap.add_argument("--no-reset", action="store_true")
    args = ap.parse_args()

    ser = open_port(args.port, not args.no_reset)
    if not wait_prompt(ser):
        print("no prompt; is the board running?", file=sys.stderr)
        return 1

    # Warm-up: the first command pays for a cold i-cache and a cold task.
    sample(ser)
    wait_prompt(ser)

    idle = [sample(ser) for _ in range(args.per_batch)]

    loaded = []
    for _ in range(args.batches):
        ser.reset_input_buffer()
        ser.write(b"run spin &\n")
        if not wait_prompt(ser, timeout=4.0):
            print("could not start the burner", file=sys.stderr)
            break
        for _ in range(args.per_batch):
            loaded.append(sample(ser))

    stats("idle", idle)
    stats("load", loaded)
    print("\nThe gap between the two is what the slice costs a task that is\n"
          "already runnable at the same priority.")
    ser.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
