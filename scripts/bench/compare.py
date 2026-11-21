#!/usr/bin/env python3
"""Run bench/bench.c and bench/bench.py on a board and compare engines.

Three engines run the same four integer kernels:

    pvm   legacy AMX VM   - csa compiles bench.c on the board
    rvm   RIR2 interpreter - csc compiles bench.c, rvm runs it
    rcc   native image     - csc + rcc produce a .tos, the loader runs it

MicroPython runs bench/bench.py (bytecode) and bench/bench_native.py
(@micropython.native) and prints the same lines.

Because `csa`/`pvm` ship in the default images and `csc`/`rvm`/`rcc` only in
rir images, the TinyOS side needs two flashes:

    # image built without rir (default): pvm
    python scripts/bench/compare.py --port COM4 --engine pvm

    # image built with configs/esp32c3-rir.conf or configs/rv32-rir.conf
    python scripts/bench/compare.py --port COM4 --engine rvm --rcc

    # MicroPython firmware on the same board, host side:
    python scripts/bench/compare.py --mpy

    python scripts/bench/compare.py --report

The scripts stage bench/bench.c into files.local/ (gitignored) and never
touch files/.  `sum=` is the checksum: a row whose sums differ from the
others was not the same workload and must not be compared.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOS = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD = os.path.join(TOS, "build", "bench")
LINE = re.compile(r"^BENCH (\w+) (ms|sum)=(-?\d+)$")


class Console:
    def __init__(self, port, dtr=False):
        import serial
        self.s = serial.Serial(port, 115200, timeout=0.1)
        # Only touch DTR when the caller asks: on a CH340 the line is wired to
        # the auto-reset circuit, and clearing it reboots the board.
        if dtr:
            self.s.dtr = True
        self.s.reset_input_buffer()

    def write(self, data):
        self.s.write(data)

    def prompt(self, timeout=30.0):
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.s.read(4096)
            if chunk:
                buf += chunk
                if buf.decode("utf-8", "replace").rstrip().endswith("tos>"):
                    break
        return buf.decode("utf-8", "replace")

    def cmd(self, line, timeout=120.0):
        self.s.reset_input_buffer()
        self.write(line.encode() + b"\r\n")
        return self.prompt(timeout)


def parse(text):
    """BENCH <name> ms=<n> / BENCH <name> sum=<n> -> {name: {ms, sum}}."""
    out = {}
    for ln in text.splitlines():
        m = LINE.match(ln.strip())
        if m:
            row = out.setdefault(m.group(1), {})
            row[m.group(2)] = int(m.group(3))
    return {k: v for k, v in out.items() if "ms" in v and "sum" in v}


def stage(name, data):
    os.makedirs(os.path.join(TOS, "files.local"), exist_ok=True)
    path = os.path.join(TOS, "files.local", name)
    with open(path, "wb") as f:
        f.write(data)
    print("staged", path)


def tos_phase(args):
    src = os.path.join(TOS, "bench", "bench.c")
    stage("bench.c", open(src, "rb").read())
    con = Console(args.port, args.dtr)
    results = {}
    try:
        con.prompt(args.timeout)
        if args.engine in ("clang", "goc32"):
            text = con.cmd("bench" + ("clang" if args.engine == "clang" else "goc"),
                           args.run_timeout)
            results[args.engine] = parse(text)
            print(text)
            return results
        if args.engine == "pvm":
            if args.compile:
                print(con.cmd("csa /bench.c /bench.amx", 180))
            text = con.cmd("pvm /bench.amx", args.run_timeout)
        else:
            if args.compile:
                print(con.cmd("csc /bench.c /bench.rir", 180))
            if args.rvm:
                text = con.cmd("rvm /bench.rir", args.run_timeout)
                results["rvm"] = parse(text)
                print(text)
            text = con.cmd("rcc /bench.rir /bin/benchc", 180)
            print(text)
            text = con.cmd("benchc", args.run_timeout)
        results[args.engine] = parse(text)
        print(text)
    finally:
        con.s.close()
    return results


def mpy_phase(args):
    results = {}
    for engine, script in (("micropython", "bench.py"),
                           ("micropython-native", "bench_native.py")):
        path = os.path.join(TOS, "bench", script)
        r = subprocess.run(["mpremote"] + args.mpremote.split() + ["run", path],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print(f"{engine}: mpremote failed\n{r.stdout}\n{r.stderr}")
            continue
        print(r.stdout)
        results[engine] = parse(r.stdout)
    return results


def save(results):
    os.makedirs(BUILD, exist_ok=True)
    for engine, rows in results.items():
        path = os.path.join(BUILD, engine + ".json")
        old = {}
        if os.path.exists(path):
            old = json.load(open(path, encoding="utf-8"))
        old.update(rows)
        with open(path, "w", encoding="utf-8") as f:
            json.dump(old, f, indent=2, sort_keys=True)
        print("wrote", path)


def report():
    files = sorted(f for f in os.listdir(BUILD) if f.endswith(".json"))
    if not files:
        print("no results in", BUILD)
        return 1
    data = {}
    for f in files:
        data[f[:-5]] = json.load(open(os.path.join(BUILD, f), encoding="utf-8"))
    kernels = ["fib", "loop", "array", "div"]
    engines = list(data)
    print("| kernel | " + " | ".join(engines) + " |")
    print("|---|" + "---:|" * len(engines))
    for k in kernels:
        cells = []
        for e in engines:
            row = data[e].get(k)
            cells.append(f"{row['ms']} ms" if row else "-")
        print(f"| {k} | " + " | ".join(cells) + " |")
    print("\nchecksums:")
    for k in kernels:
        sums = {e: data[e].get(k, {}).get("sum") for e in engines}
        ok = len(set(v for v in sums.values() if v is not None)) == 1
        print(f"  {k}: {'ok' if ok else 'MISMATCH'} {sums}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", help="serial port of the TinyOS console")
    ap.add_argument("--dtr", action="store_true", help="assert DTR (USB CDC)")
    ap.add_argument("--engine",
                    choices=("pvm", "rvm", "rcc", "clang", "goc32"),
                    default=None,
                    help="engine to measure; clang/goc32 are native images "
                         "built by the host (see docs/BENCH.md)")
    ap.add_argument("--rvm", action="store_true",
                    help="with --engine rcc: also measure rvm on the same image")
    ap.add_argument("--no-compile", dest="compile", action="store_false",
                    help="skip the on-board csa/csc step (image already built)")
    ap.add_argument("--mpy", action="store_true",
                    help="run MicroPython over mpremote instead of the board")
    ap.add_argument("--mpremote", default="", help="extra mpremote args")
    ap.add_argument("--report", action="store_true", help="print the table")
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--run-timeout", type=float, default=300.0)
    args = ap.parse_args()

    if args.report:
        return report()
    if args.mpy:
        save(mpy_phase(args))
        return 0
    if not args.port or not args.engine:
        ap.error("need --port and --engine, or --mpy, or --report")
    save(tos_phase(args))
    return 0


if __name__ == "__main__":
    sys.exit(main())
