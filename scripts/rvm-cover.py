#!/usr/bin/env python3
"""Run the C Script trio (csc -> rvm -> rcc -> native) on QEMU.

The image carries the scripts as /tests/<name>.c (files/tests/); this builds a
QEMU target with the on-board tools and runs all four stages for each of them:

    csc /tests/<name>.c /tests/<name>.rir
    rvm /tests/<name>.rir
    rcc /tests/<name>.rir /bin/<name>
    <name>

The interpreter's output (minus rvm's "result=..." trailer) and the native
program's output must both equal tests/onboard/<name>.expected, and therefore
each other.  That is the end-to-end coverage: one script source, both backends.

This is the pre-board half of the same chain the C3 and RP2040 acceptance run:
`cover` is the feature matrix, `state` the state-machine demo, `ready` the
yield()/ready(fd) natives.  Run it before flashing a board.

    python scripts/rvm-cover.py              # rv32 (default), all three scripts
    python scripts/rvm-cover.py --target arm # thumb2 backend
    python scripts/rvm-cover.py --only cover
"""
import argparse
import os
import subprocess
import sys

SCRIPTS = ("cover", "state", "ready")


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
    ap.add_argument("--only", default=None, choices=SCRIPTS,
                    help="run one script instead of all three")
    ap.add_argument("--keep-log", action="store_true")
    args = ap.parse_args()

    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    config = os.path.join(root, "configs", args.target + "-rir.conf")
    scripts = (args.only,) if args.only else SCRIPTS
    expected = {}
    for name in scripts:
        path = os.path.join(root, "tests", "onboard", name + ".expected")
        expected[name] = [l.strip() for l in open(path, encoding="utf-8") if l.strip()]

    rc, out = run(["go", "run", "./cmd/tos", "build", "-config", config], root)
    if rc != 0:
        print(out[-4000:])
        print("FAIL: image build")
        return 1

    cmds = ",".join(
        ["csc /tests/%s.c /tests/%s.rir" % (n, n) for n in scripts] +
        ["rvm /tests/%s.rir" % n for n in scripts] +
        ["rcc /tests/%s.rir /bin/%s" % (n, n) for n in scripts] +
        list(scripts))
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
    ok = True
    for name in scripts:
        want = expected[name]
        rvm = block_after(lines, "rvm /tests/%s.rir" % name)
        nat = block_after(lines, name)
        if rvm is None or nat is None:
            print("FAIL: %s: could not find the rvm/native output blocks" % name)
            print(out[-5000:])
            return 1
        rvm_clean, nat_clean = clean(rvm), clean(nat)
        if "csc: /tests/%s.c -> /tests/%s.rir" % (name, name) not in out:
            print("FAIL: %s: csc did not report the RIR output" % name)
            ok = False
        if "rcc:" not in out:
            print("FAIL: %s: rcc did not report the native image" % name)
            ok = False
        if rvm_clean != want:
            print("FAIL: %s: rvm output differs from %s.expected" % (name, name))
            print("  got:", rvm_clean)
            print("  want:", want)
            ok = False
        if nat_clean != want:
            print("FAIL: %s: native output differs from %s.expected" % (name, name))
            print("  got:", nat_clean)
            print("  want:", want)
            ok = False
        if rvm_clean != nat_clean:
            print("FAIL: %s: rvm and native outputs differ" % name)
            ok = False
        if ok:
            print("PASS: %-5s csc -> rvm -> rcc -> native, %d lines, both backends match"
                  % (name, len(want)))

    if not ok:
        print(out[-5000:])
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
