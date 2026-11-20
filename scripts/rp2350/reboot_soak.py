#!/usr/bin/env python3
"""Reboot soak: does `reboot` always come back, quickly, on a fresh machine.

This exists because the RV32 SMP port spent a while taking 33.5 s to come back
in about half of all attempts - a lock-order deadlock inside the reboot path
(HISTORY 85) - and nothing in the tree would have noticed: the boot gate waited
for a prompt with a generous window, and a 34 s cycle and a 0.5 s cycle both
looked the same once they eventually answered.

Two criteria, and the second is the one that matters:

  1. the UART0 prompt arrives within --max seconds (default 2.0);
  2. the uptime the new shell reports is small, i.e. the machine really did
     reset.  Waiting for a prompt cannot tell a successful reset from one that
     never happened - the old prompt is still there and the "boot" being
     measured is the previous one.  That is not hypothetical: the first version
     of gate_boot.py passed 15/15 without resetting anything.

Usage:
    python scripts/rp2350/reboot_soak.py -n 20
    python scripts/rp2350/reboot_soak.py -n 20 --csv build/reboot-soak.csv
"""

import argparse
import csv
import re
import sys
import time

import serial

PROMPT = b"tos>"
UPTIME_RE = re.compile(rb"uptime:\s*([0-9.]+)\s*s")


def read_until_prompt(port, window):
    """Read until the prompt appears.  Returns (seconds or None, bytes)."""
    t0 = time.time()
    buf = b""
    while time.time() - t0 < window:
        chunk = port.read(4096)
        if not chunk:
            continue
        buf += chunk
        if buf.rstrip().endswith(PROMPT):
            return time.time() - t0, buf
    return None, buf


def command(port, text, window, tries=2, want=None):
    """Send a line, return (answered, text received).

    Retried once on no answer.  The first command written immediately after a
    boot's prompt is occasionally swallowed - the prompt is printed by msh
    before its first getline(), so a byte that lands in between is echoed by
    the tty layer and then discarded.  That is a race in the harness, not a
    board fault, and one retry separates the two: a machine that is really
    wedged fails both attempts.
    """
    text_seen = ""
    for _ in range(tries):
        port.reset_input_buffer()
        port.write((text + "\r").encode())
        dt, buf = read_until_prompt(port, window)
        text_seen = buf.decode("utf-8", "replace")
        ok = dt is not None and (want is None or want.search(text_seen.encode(
            "utf-8", "replace")) is not None)
        if ok:
            return True, text_seen
        time.sleep(0.2)
    return False, text_seen


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uart", default="COM19")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("-n", "--cycles", type=int, default=10)
    ap.add_argument("--max", type=float, default=2.0,
                    help="a cycle slower than this fails")
    ap.add_argument("--window", type=float, default=45.0,
                    help="give up on a cycle after this long")
    ap.add_argument("--csv", default="")
    args = ap.parse_args()

    uart = serial.Serial(args.uart, args.baud, timeout=0.05)
    uart.dtr = False
    uart.rts = False
    time.sleep(0.3)

    # A bare newline, to make an idle shell print its prompt again: reading
    # alone finds nothing on a board that is already sitting at one.
    if not command(uart, "", 8.0)[0]:
        print("no prompt on %s to start from" % args.uart)
        return 2

    rows = []
    failures = 0
    for i in range(args.cycles):
        uart.reset_input_buffer()
        uart.write(b"reboot\r\n")
        dt, _ = read_until_prompt(uart, args.window)
        slow = dt is None or dt > args.max

        uptime = None
        if dt is not None:
            # `want` makes the retry cover a half-delivered answer too, not
            # just silence: the prompt can arrive while the line above it is
            # still in flight, and a run that retries only on "no prompt at
            # all" still loses a cycle to that.  A wedged shell fails all three.
            ok, text = command(uart, "uptime", 6.0, tries=3, want=UPTIME_RE)
            m = UPTIME_RE.search(text.encode("utf-8", "replace"))
            if m:
                uptime = float(m.group(1))

        fresh = uptime is not None and uptime <= args.max
        verdict = "PASS" if (not slow and fresh) else "FAIL"
        if verdict == "FAIL":
            failures += 1
        rows.append({
            "cycle": i + 1,
            "prompt_s": "" if dt is None else round(dt, 2),
            "uptime_s": "" if uptime is None else round(uptime, 2),
            "verdict": verdict,
        })
        print("[%2d/%d] %s  prompt=%s  uptime=%s"
              % (i + 1, args.cycles, verdict,
                 "NEVER" if dt is None else "%.2f s" % dt,
                 "?" if uptime is None else "%.2f s" % uptime))
        sys.stdout.flush()
        time.sleep(0.3)

    uart.close()

    good = [r["prompt_s"] for r in rows if r["prompt_s"] != ""]
    print()
    print("%d/%d cycles clean" % (args.cycles - failures, args.cycles))
    if good:
        print("  prompt: min %.2f s, max %.2f s, mean %.2f s"
              % (min(good), max(good), sum(good) / len(good)))
    print("  judgement: prompt within %.1f s AND the shell's own uptime reset"
          % args.max)

    if args.csv and rows:
        with open(args.csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
        print("  csv: %s" % args.csv)

    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
