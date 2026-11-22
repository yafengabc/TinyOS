#!/usr/bin/env python3
"""Run the C Script trio (csc -> rvm -> rcc) against tests/onboard/cover.c.

The image carries the script as /tests/cover.c (files/tests/cover.c); this
builds a QEMU target with the on-board tools and runs all three stages:

    csc /tests/cover.c /tests/cover.rir
    rvm /tests/cover.rir
    rcc /tests/cover.rir /bin/cover
    cover

The interpreter's output (minus rvm's "result=..." trailer) and the native
program's output must both equal tests/onboard/cover.expected, and therefore
each other.  That is the end-to-end coverage: one script, both backends.

    python scripts/rvm-cover.py            # rv32 (default)
    python scripts/rvm-cover.py --target arm
"""
import argparse
import os
import subprocess
import sys


def run(cmd, cwd):
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    return p.returncode, (p.stdout or "") + (p.stderr or "")


def block_after(lines, marker):
    """Lines between the command echo `tos> marker...` and the next prompt."""
    start = None
    for i, line in enumerate(lines):
        if line.startswith("tos> " + marker):
            start = i + 1
            break
    if start is None:
        return None
    out = []
    for line in lines[start:]:
        if line.startswith("tos> "):
            break
        out.append(line.rstrip())
    return out


def clean(lines):
    return [l.strip() for l in lines if l.strip() and not l.strip().startswith("result=")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", default="rv32", choices=("rv32", "arm"))
    ap.add_argument("--keep-log", action="store_true")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    config = os.path.join(root, "configs", args.target + "-rir.conf")
    expected_path = os.path.join(root, "tests", "onboard", "cover.expected")
    expected = [l.strip() for l in open(expected_path, encoding="utf-8") if l.strip()]

    rc, out = run(["go", "run", "./cmd/tos", "build", "-config", config], root)
    if rc != 0:
        print(out[-4000:])
        print("FAIL: image build")
        return 1

    cmds = ("csc /tests/cover.c /tests/cover.rir,"
            "rvm /tests/cover.rir,"
            "rcc /tests/cover.rir /bin/cover,"
            "cover")
    rc, out = run(["go", "run", "./cmd/tostest", "smoke",
                   "-target", args.target, "-commands", cmds,
                   "-commandwait-ms", "120000"], root)
    if args.keep_log:
        open(os.path.join(root, "build", "rvm-cover.log"), "w",
             encoding="utf-8").write(out)
    if rc != 0 or "SMOKE TEST OK" not in out:
        print(out[-5000:])
        print("FAIL: smoke run")
        return 1

    lines = out.splitlines()
    rvm = block_after(lines, "rvm /tests/cover.rir")
    nat = block_after(lines, "cover /")
    # The native command's echo is exactly "tos> cover" with no argument.
    if nat is None:
        nat = block_after(lines, "cover")
    if rvm is None or nat is None:
        print(out[-5000:])
        print("FAIL: could not find the rvm/native output blocks")
        return 1

    rvm_clean, nat_clean = clean(rvm), clean(nat)
    ok = True
    if "csc: /tests/cover.c -> /tests/cover.rir" not in out:
        print("FAIL: csc did not report the RIR output")
        ok = False
    if "rcc:" not in out:
        print("FAIL: rcc did not report the native image")
        ok = False
    if rvm_clean != expected:
        print("FAIL: rvm output differs from cover.expected")
        print("  got:", rvm_clean)
        print("  want:", expected)
        ok = False
    if nat_clean != expected:
        print("FAIL: native output differs from cover.expected")
        print("  got:", nat_clean)
        print("  want:", expected)
        ok = False
    if rvm_clean != nat_clean:
        print("FAIL: rvm and native outputs differ")
        ok = False

    if not ok:
        print(out[-5000:])
        return 1
    print("PASS: csc -> rvm -> rcc -> native, %d lines, both backends match"
          % len(expected))
    return 0


if __name__ == "__main__":
    sys.exit(main())
