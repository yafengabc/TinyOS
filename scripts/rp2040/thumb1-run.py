#!/usr/bin/env python3
"""Run the Thumb-1 test set on an RP2040 and compare with the host interpreter.

Run scripts/rp2040/thumb1-prep.py first: it stages build/thumb1/expected/*.txt
and files.local/bin/t1-*, and builds the image.

    python scripts/rp2040/thumb1-run.py --port COM19
    python scripts/rp2040/thumb1-run.py --port COM19 --set extra
    python scripts/rp2040/thumb1-run.py --replay build/thumb1/console.log

Every strict program is compared line by line against the interpreter's output.
Programs whose output depends on the board (peripherals, timing) or on a
previous command (capture/run/file IO) are printed for the eye and marked
INFO, not PASS/FAIL.  The transcript is written to build/thumb1/ and the exit
code is 0 only when every strict program matched.
"""

import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOS = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD = os.path.join(TOS, "build", "thumb1")
EXPECTED = os.path.join(BUILD, "expected")

CORE_STRICT = [
    "arith", "arrays2", "bigcode", "bigir", "bloop3", "copy", "deep",
    "float3", "fmt", "fold", "funcs", "mixed", "mod", "ops", "sa3", "sb3",
    "sieve3", "str", "strings2", "struct3",
]
CORE_EYEBALL = ["capture1", "dir1", "exec1", "file1", "native"]
EXTRA_STRICT = [
    "bigbody", "bigswitch", "cmp2", "floatfold", "flow", "flow2", "globals2",
    "hugeir", "loops2", "many20", "many25", "many32", "mixbig", "spill",
    "sret", "switch", "switch2",
]
EXTRA_EYEBALL = ["benchfib", "benchloop"]

# What the eyeball programs should show (checked by a human, not scored).
HINTS = {
    "capture1": "a number (0 or a small capture length), no fault",
    "dir1": "three lines: 1, 0, -1 (isdir of /bin, a file, a missing path)",
    "exec1": "the child's 'hello from ring 3!' line, then 0",
    "file1": "file IO round trip: 1, 24, 0, 0, a line of text, 0, 0",
    "native": "no fault; the values depend on the board's peripherals",
    "benchfib": "a time in ms, then 46368",
    "benchloop": "a time in ms, then -2014260032",
}


class Replay:
    """Feed a captured console transcript instead of a serial port."""

    def __init__(self, path, prompt):
        text = open(path, encoding="utf-8", errors="replace").read()
        self.chunks = re.split(r"(?<=tos> )", text)
        self.i = 0
        self.prompt = prompt

    def write(self, data):
        pass

    def read_until_prompt(self, timeout):
        if self.i >= len(self.chunks):
            return ""
        out = self.chunks[self.i]
        self.i += 1
        return out


class SerialConsole:
    def __init__(self, port, dtr):
        import serial  # only needed on the hardware path
        self.s = serial.Serial(port, 115200, timeout=0.1)
        self.s.dtr = bool(dtr)
        self.s.reset_input_buffer()

    def write(self, data):
        self.s.write(data)

    def read_until_prompt(self, timeout):
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.s.read(4096)
            if chunk:
                buf += chunk
                if buf.decode("utf-8", "replace").rstrip().endswith("tos>"):
                    break
        return buf.decode("utf-8", "replace")


def run_one(con, name, timeout):
    con.write(("t1-" + name).encode() + b"\r\n")
    text = con.read_until_prompt(timeout)
    # drop the echoed command and the trailing prompt
    body = text
    m = re.search(r"t1-" + re.escape(name) + r"\r?\n(.*)", body, re.S)
    if m:
        body = m.group(1)
    body = re.sub(r"tos>\s*$", "", body)
    return [ln.rstrip("\r") for ln in body.splitlines()]


def expected_lines(name):
    path = os.path.join(EXPECTED, name + ".txt")
    if not os.path.exists(path):
        return None
    return [ln.rstrip() for ln in open(path, encoding="utf-8").read().splitlines()]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", help="serial port, e.g. COM19")
    ap.add_argument("--dtr", action="store_true", help="assert DTR (USB CDC)")
    ap.add_argument("--replay", help="replay a captured transcript instead")
    ap.add_argument("--set", choices=("all", "core", "extra"), default="all")
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()
    if not args.port and not args.replay:
        ap.error("need --port or --replay")

    if args.set == "core":
        strict, eyeball = CORE_STRICT, CORE_EYEBALL
    elif args.set == "extra":
        strict, eyeball = EXTRA_STRICT, EXTRA_EYEBALL
    else:
        strict = CORE_STRICT + EXTRA_STRICT
        eyeball = CORE_EYEBALL + EXTRA_EYEBALL
    missing = [n for n in strict if expected_lines(n) is None]
    if missing:
        print("missing expected output for:", " ".join(missing))
        print("run scripts/rp2040/thumb1-prep.py first")
        return 2

    if args.replay:
        con = Replay(args.replay, "tos>")
    else:
        try:
            con = SerialConsole(args.port, args.dtr)
        except Exception as e:                                  # noqa: BLE001
            print(f"cannot open {args.port}: {e}")
            return 2

    os.makedirs(BUILD, exist_ok=True)
    log_path = os.path.join(BUILD, "console-%s.log" % time.strftime("%Y%m%d-%H%M%S"))
    log = open(log_path, "w", encoding="utf-8")
    bad = 0

    def emit(line):
        print(line)
        log.write(line + "\n")

    try:
        emit(con.read_until_prompt(args.timeout))
        for name in strict:
            lines = run_one(con, name, args.timeout)
            want = expected_lines(name)
            ok = lines == want
            emit("%s  %-12s %s" % ("PASS" if ok else "FAIL", name,
                                   "" if ok else f"want={want} got={lines}"))
            if not ok:
                bad += 1
        for name in eyeball:
            lines = run_one(con, name, args.timeout)
            emit("INFO  %-12s %s" % (name, " | ".join(lines[:6])))
            emit("      expect: " + HINTS.get(name, "as documented"))
        emit("")
        emit("strict %d/%d, eyeball %d listed, transcript %s"
             % (len(strict) - bad, len(strict), len(eyeball), log_path))
    finally:
        log.close()
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
