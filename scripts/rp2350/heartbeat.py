#!/usr/bin/env python3
"""Six-item heartbeat for the RP2350 targets (ATK, Pico 2 and friends).

This is the board-level minimum from docs/TESTING.md section 5.0: it does not
claim the full acceptance table, only that the image boots, keeps time, reads
and writes the filesystem, can exec a user program, and survives a user fault.

    python scripts/rp2350/heartbeat.py -p COM19 --target rpi-pico2
    python scripts/rp2350/heartbeat.py -p COM17 --dtr --target atk-dnr2350am-smp

The board must already be running the image; flashing is a separate step
(scripts/rp2350/flash.py).  The six checks are the same as the RP2040 port's
scripts/rp2040/heartbeat.py on purpose: the point of a heartbeat is that every
board answers the same questions the same way.

Output is one line per item and a final `HEARTBEAT OK n/n` (exit 0) or
`HEARTBEAT FAIL n/n` (exit 1).
"""

import argparse
import re
import sys
import time

import serial


class Console:
    def __init__(self, port, dtr=False):
        self.s = serial.Serial(port, 115200, timeout=0.1)
        self.s.dtr = bool(dtr)
        self.s.reset_input_buffer()

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def prompt(self, timeout=10.0):
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.s.read(4096)
            if chunk:
                buf += chunk
                if buf.decode("utf-8", "replace").rstrip().endswith("tos>"):
                    break
        return buf.decode("utf-8", "replace")

    def cmd(self, line, timeout=10.0):
        self.s.write(line.encode() + b"\r\n")
        return self.prompt(timeout)


def uptime_seconds(text):
    m = re.search(r"uptime:\s*([0-9]+(?:\.[0-9]+)?)\s*s", text)
    return float(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", required=True)
    ap.add_argument("--target", default="rpi-pico2",
                    help="recorded in the report; the script itself is board-agnostic")
    ap.add_argument("--dtr", action="store_true", help="assert DTR (USB CDC console)")
    args = ap.parse_args()

    try:
        con = Console(args.port, dtr=args.dtr)
    except serial.SerialException as e:
        print("cannot open %s: %s" % (args.port, e))
        return 2

    results = []

    def check(name, ok, detail=""):
        results.append((name, bool(ok), detail))
        print("%s  %-26s  %s" % ("PASS" if ok else "FAIL", name, detail))
        return bool(ok)

    try:
        boot = con.cmd("")
        check("1 boot to a shell", "tos>" in boot, "prompt seen")

        u1t = con.cmd("uptime")
        t1 = time.monotonic()
        u1 = uptime_seconds(u1t)
        time.sleep(1.0)
        u2t = con.cmd("uptime")
        t2 = time.monotonic()
        u2 = uptime_seconds(u2t)
        delta = (u2 - u1) if (u1 is not None and u2 is not None) else None
        wall = t2 - t1
        # The two uptime reads are separated by the host sleep *and* the serial
        # round trip, so compare the board's delta with the measured wall time
        # rather than assuming the host sleep is the whole interval.
        ok = delta is not None and abs(delta - wall) <= 0.1
        check("2 time advances 1 s", ok,
              "delta=%.3f s, wall=%.3f s" % (delta, wall)
              if delta is not None else "no uptime line")

        ls = con.cmd("ls /bin")
        check("3 filesystem read", bool(re.search(r"\bmsh\b", ls)), "ls /bin")

        hello = con.cmd("hello")
        check("4 user program", "hello from ring 3!" in hello, "hello")

        con.cmd("wfile hb.tmp x")
        cat = con.cmd("cat hb.tmp")
        rm = con.cmd("rm hb.tmp")
        check("5 filesystem write-back", "x" in cat and "tos>" in rm,
              "wfile/cat/rm")

        crash = con.cmd("crash")
        check("6 fault isolation", "tos>" in crash and "KERNEL PANIC" not in crash,
              "crash returned to prompt")
    finally:
        con.close()

    passed = sum(1 for _, ok, _ in results if ok)
    total = len(results)
    print("\n%s %d/%d" % ("HEARTBEAT OK" if passed == total else "HEARTBEAT FAIL",
                          passed, total))
    print("target: %s  port: %s" % (args.target, args.port))
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
