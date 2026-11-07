#!/usr/bin/env python3
"""Drive the RP2350 SMP checks over the console and report one table.

Three things are being measured and they must not be confused:

  * a `BAD` line from `smp -w`   - the kernel handed the task a frame it did
    not write (docs/OPEN-ITEMS.md §22), an out-of-range CPU id;
  * an exit code that is not a plausible block count - usually a stack
    address, i.e. the same lost-frame family reaching `wait()`;
  * no prompt back within the timeout - the parent is parked in
    ST_WAIT_CHILD and nobody will ever wake it (§23.2).  This is the one the
    block_locked() fix is supposed to remove, and the only one that needs a
    power cycle / Ctrl-C to recover from.

`smp -s <n> <ms>` prints one summary line because the per-worker line goes
missing under console contention (#78); the exit codes are the block counts.

    python scripts/rp2350/smptest.py -p COM7                 # full battery
    python scripts/rp2350/smptest.py -p COM7 --only hang     # just `-s 4`
    python scripts/rp2350/smptest.py -p COM7 --func          # + shell checks
"""

import argparse
import os
import re
import sys
import time

import serial

MARK = b"tos> "
BAD = ("KERNEL PANIC", "System halted", "[spin]")

# A worker's block count in 500 ms is thousands, never millions; a code above
# this is an address or a stray word, not a count.
MAX_PLAUSIBLE = 2_000_000


class Board:
    def __init__(self, port, baud=115200, timeout=0.05):
        self.s = serial.Serial(port, baud, timeout=timeout)
        self.s.dtr = False
        self.s.rts = False

    def close(self):
        self.s.close()

    def _drain(self, seconds, until_mark):
        buf = bytearray()
        end = time.time() + seconds
        while time.time() < end:
            data = self.s.read(4096)
            if not data:
                continue
            buf.extend(data)
            if until_mark and MARK in bytes(buf):
                return bytes(buf), True
        return bytes(buf), MARK in bytes(buf)

    def sync(self, timeout=8.0):
        """Get a prompt: a bare newline makes the shell reprint it."""
        self.s.reset_input_buffer()
        self.s.write(b"\r\n")
        self.s.flush()
        return self._drain(timeout, True)

    def cmd(self, line, timeout=20.0):
        self.s.write(line.encode() + b"\r\n")
        self.s.flush()
        return self._drain(timeout, True)

    def ctrl_c(self):
        self.s.write(b"\x03")
        self.s.flush()
        return self._drain(3.0, True)


RESULT = re.compile(rb"smp -s result:((?:\s+w\d+=-?\d+)+)")


def parse(out):
    m = RESULT.search(out)
    if not m:
        return None
    return [int(x) for x in re.findall(rb"w\d+=(-?\d+)", m.group(1))]


def one_run(bd, line, timeout):
    """One `smp -s` invocation.  Returns (verdict, codes, note)."""
    out, prompted = bd.cmd(line, timeout)
    text = out.decode("utf-8", "replace")

    if any(b in text for b in BAD):
        for b in BAD:
            if b in text:
                return "FAIL", None, b
    if not prompted:
        # Recover: the blocked parent cannot be woken, but ^C goes for the
        # foreground task, which is the smp that is waiting.
        bd.ctrl_c()
        return "HANG", None, "no prompt"
    if b"BAD" in out:
        first = [ln for ln in text.splitlines() if "BAD" in ln]
        return "BAD-FRAME", parse(out), first[0].strip() if first else ""

    codes = parse(out)
    if codes is None:
        return "FAIL", None, "no result line"
    for i, c in enumerate(codes):
        if c < 0 or c > MAX_PLAUSIBLE:
            return "BAD-CODE", codes, f"w{i}={c}"
    return "ok", codes, ""

def battery(bd, only):
    runs = []
    if only in (None, "hang"):
        runs += [("smp -s 4 500", 25.0)] * 5
    if only in (None, "rate"):
        runs += [("smp -s 2 500", 20.0)] * 3
        runs += [("smp -s 1 500", 20.0)] * 2
    if only in (None, "topology"):
        runs += [("smp", 30.0)] * 1

    results = []
    for i, (line, tmo) in enumerate(runs):
        if line == "smp":
            # No -s: this one reports the topology and the per-worker lines
            # instead of an exit-code summary, so only its liveness counts.
            out, prompted = bd.cmd(line, tmo)
            text = out.decode("utf-8", "replace")
            good = prompted and "CPU(s)" in text and "worker" in text \
                and not any(b in text for b in BAD)
            verdict, codes, note = ("ok", None, "") if good else \
                ("FAIL", None, "topology")
            if not prompted:
                bd.ctrl_c()
                verdict, note = "HANG", "no prompt"
        else:
            verdict, codes, note = one_run(bd, line, tmo)
        results.append((line, verdict, codes, note))
        shown = "-" if codes is None else " ".join(str(c) for c in codes)
        print(f"  [{i + 1}/{len(runs)}] {line:<14} {verdict:<10} {shown}"
              + (f"  ({note})" if note else ""))
        if verdict == "HANG":
            # The queue is serialised behind a task that will never run:
            # nothing else in this battery would mean anything.
            print("  stopping: the console is wedged")
            break
    return results


FUNC = [
    ("echo hello-smp", b"hello-smp"),
    ("ls bin", b"smp"),
    ("free", b"kernel"),
    ("ps", b""),
    ("cat readme.txt | wc", b""),
    ("py demo.py", b"sum"),
    ("sleep 300", b""),
]


def functional(bd):
    ok = 0
    for line, want in FUNC:
        out, prompted = bd.cmd(line, 25.0)
        text = out.decode("utf-8", "replace")
        good = prompted and want in out and not any(b in text for b in BAD)
        print(f"  [{'ok' if good else 'FAIL'}] {line}")
        if not good:
            for ln in text.splitlines():
                if ln.strip():
                    print("        " + ln.rstrip())
        ok += 1 if good else 0
    return ok, len(FUNC)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="auto")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--only", choices=["hang", "rate", "topology"], default=None)
    ap.add_argument("--func", action="store_true", help="also run shell checks")
    args = ap.parse_args()

    port = args.port
    if port == "auto":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from flash import cdc_port
        port = cdc_port()
        if not port:
            print("no TinyOS USB CDC port found; pass -p COMxx", file=sys.stderr)
            return 2

    try:
        bd = Board(port, args.baud)
    except serial.SerialException as e:
        print(f"cannot open {port}: {e}", file=sys.stderr)
        return 2

    print(f"port {port}")
    boot, prompted = bd.sync(10.0)
    if not prompted:
        print("no prompt; is the board up?", file=sys.stderr)
        print(boot.decode("utf-8", "replace"), file=sys.stderr)
        return 2

    print("smp battery:")
    results = battery(bd, args.only)

    if args.func:
        print("functional checks:")
        ok, n = functional(bd)
        print(f"  {ok}/{n} passed")

    bad = [r for r in results if r[1] != "ok"]
    print(f"\n{len(results) - len(bad)}/{len(results)} clean")
    for line, verdict, codes, note in bad:
        print(f"  {verdict:<10} {line}  {codes}  {note}")

    # A rate run is the only one whose numbers are compared across builds.
    rates = [c for line, v, c, _ in results if line == "smp -s 2 500"
             and v == "ok" and c]
    if rates:
        flat = [x for run in rates for x in run]
        print(f"smp -s 2 500 block counts: {flat} "
              f"(min {min(flat)}, max {max(flat)})")

    bd.close()
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
