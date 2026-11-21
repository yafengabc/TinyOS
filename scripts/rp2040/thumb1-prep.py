#!/usr/bin/env python3
"""Build the Thumb-1 (ARMv6-M) test set and the RP2040 image.

Run this on the machine that has the toolchain (Go, a host C compiler,
Python).  It does everything except flashing:

  1. builds `csc` and `rcc` (RCC_ARCH_THUMB1) for the host,
  2. compiles every program in tinyos-script/tests into a .tos,
  3. records the expected output of each program from the host interpreter
     into build/thumb1/expected/<name>.txt,
  4. stages the images into files.local/bin/t1-<name>,
  5. builds configs/rpi-pico.conf and reports the .uf2 path.

    python scripts/rp2040/thumb1-prep.py
    python scripts/rp2040/thumb1-prep.py --script ../tinyos-script --cc clang

Nothing is written outside build/ and files.local/.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOS = os.path.abspath(os.path.join(HERE, "..", ".."))
BUILD = os.path.join(TOS, "build", "thumb1")

# `--set` splits the corpus in three groups for convenience, not because of
# space: the build grows the littlefs image (doubling up to the board's
# partition) when the staged set does not fit, so `all` needs no board.conf
# edit.
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


def run(cmd, **kw):
    print("+", " ".join(cmd), flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        raise SystemExit(f"failed: {' '.join(cmd)}")
    return r.stdout


def run_allow_exit(cmd):
    """Run a program that is expected to exit non-zero (sb3 faults on purpose)."""
    print("+", " ".join(cmd), flush=True)
    return subprocess.run(cmd, capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--script", default=os.path.join("..", "tinyos-script"),
                    help="path to the tinyos-script checkout")
    ap.add_argument("--cc", default=os.environ.get("CC", "gcc"))
    ap.add_argument("--skip-build", action="store_true",
                    help="reuse existing build/thumb1 binaries")
    ap.add_argument("--skip-tos", action="store_true",
                    help="reuse existing build/thumb1 images")
    ap.add_argument("--set", choices=("all", "core", "extra"), default="all",
                    help="which programs to stage (all fits in one flash now)")
    ap.add_argument("--arch", choices=("thumb1", "thumb2"), default="thumb1",
                    help="target core: RP2040 ARMv6-M or RP2350 ARMv7-M")
    ap.add_argument("--config", default=None,
                    help="TinyOS config (default: rpi-pico[-rir] or rpi-pico2-quiet)")
    ap.add_argument("--source", action="store_true",
                    help="stage the .c files and let the board compile them "
                         "(needs a rir image with csc/rvm/rcc; peak window is "
                         "one tool, not the sum)")
    args = ap.parse_args()

    src = os.path.abspath(args.script)
    if not os.path.isdir(os.path.join(src, "tests")):
        raise SystemExit(f"{src}: no tests/ directory")
    exe = ".exe" if os.name == "nt" else ""
    os.makedirs(BUILD, exist_ok=True)
    exp_dir = os.path.join(BUILD, "expected")
    os.makedirs(exp_dir, exist_ok=True)
    bin_dir = os.path.join(TOS, "files.local", "bin")
    os.makedirs(bin_dir, exist_ok=True)

    csc = os.path.join(BUILD, "csc" + exe)
    rvm = os.path.join(BUILD, "rvm" + exe)
    rcc = os.path.join(BUILD, "rcc-" + args.arch + exe)

    if not args.skip_build:
        arch_def = ("-DRCC_ARCH_THUMB1" if args.arch == "thumb1"
                    else "-DRCC_ARCH_THUMB2")
        rcc_name = "rcc-" + args.arch
        for name, extra in (("csc", []), ("rvm", []),
                            (rcc_name, [arch_def])):
            sub = "rcc.c" if name.startswith("rcc") else name + ".c"
            path = os.path.join(src, "src", "rcc", sub) if name.startswith("rcc") \
                else os.path.join(src, "src", sub)
            run([args.cc, "-std=gnu11", "-O2", "-I", os.path.join(src, "include"),
                 *extra, path, "-o", os.path.join(BUILD, name + exe)])

    if args.set == "core":
        names = CORE_STRICT + CORE_EYEBALL
    elif args.set == "extra":
        names = EXTRA_STRICT + EXTRA_EYEBALL
    else:
        names = CORE_STRICT + EXTRA_STRICT + CORE_EYEBALL + EXTRA_EYEBALL
    made, skipped = [], []
    for n in names:
        source = os.path.join(src, "tests", n + ".c")
        if not os.path.exists(source):
            skipped.append((n, "no source"))
            continue
        rir = os.path.join(BUILD, n + ".rir")
        tos = os.path.join(BUILD, n + ".tos")
        if not args.skip_tos:
            run([csc, source, rir])
            if not args.source:
                run([rcc, rir, tos])
        if not args.source and not os.path.exists(tos):
            skipped.append((n, "no .tos"))
            continue
        # expected output: the interpreter, minus its summary line.  sb3 stops
        # with "bounds" and exit 1 by design, so the exit code is not checked.
        out = run_allow_exit([rvm, rir])
        lines = out.splitlines()
        if lines and lines[-1].startswith("result="):
            lines = lines[:-1]
        with open(os.path.join(exp_dir, n + ".txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + ("\n" if lines else ""))
        if args.source:
            # 84 KiB of .c instead of 260 KiB of images, and the board exercises
            # csc -> rvm -> rcc -> exec for every program.
            shutil.copyfile(source, os.path.join(TOS, "files.local", "t1-" + n + ".c"))
        else:
            shutil.copyfile(tos, os.path.join(bin_dir, "t1-" + n))
        made.append(n)

    # A staged image built for the wrong core faults on the first 32-bit
    # instruction.  Check the ARMv6-M subset before anyone flashes this.
    if args.arch == "thumb1" and made and not args.source:
        check = os.path.join(HERE, "v6mcheck.py")
        staged = [os.path.join(bin_dir, "t1-" + n) for n in made]
        r = subprocess.run([sys.executable, check] + staged,
                           capture_output=True, text=True)
        print(r.stdout.strip())
        if r.returncode != 0:
            raise SystemExit("refusing to continue: non-ARMv6-M images staged")

    print(f"\nstaged {len(made)} images in files.local/bin")
    for n, why in skipped:
        print(f"  skipped {n}: {why}")

    if args.config:
        config = args.config
    elif args.arch == "thumb1":
        config = "configs/rpi-pico-rir.conf" if args.source else "configs/rpi-pico.conf"
    else:
        config = "configs/rpi-pico2-quiet.conf"
    run(["go", "run", "./cmd/tos", "build", "-config", config], cwd=TOS)
    board = "rpi-pico" if args.arch == "thumb1" else "rpi-pico2"
    uf2 = os.path.join(TOS, "build", "tinyos-%s.uf2" % board)
    print("\nimage:", uf2 if os.path.exists(uf2) else "(check build/ output)")
    print("arch:", args.arch, " set:", args.set, " programs:", len(made))
    if args.source:
        print("source mode: the board compiles each t1-*.c (csc -> rvm -> rcc)")
    print("next: flash build/tinyos-rpi-pico.uf2, then")
    print("      python scripts/rp2040/thumb1-run.py --port COMx --set %s"
          % args.set)
    return 0


if __name__ == "__main__":
    sys.exit(main())
