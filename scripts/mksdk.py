#!/usr/bin/env python3
"""Assemble the mini user-program SDK.

The SDK is the part of the tree that a program needs plus toscc:

    tinyos-sdk/
      bin/toscc[.exe]        (and tosput[.exe] when given)
      include/*.h            the kernel/user ABI headers
      kernel/types.h         the one kernel header user code includes
      user/                  lib.c, syscall.c, the numeric runtimes, crt0, user.ld
      arch/<arch>/usys.h     the syscall instruction and register ABI
      examples/hello.c
      README.md

`kernel/types.h` is the base every include/*.h starts from, and it is also
what makes toscc recognise the tree as a TinyOS root (`user/lib.c`,
`user/user.ld`, `include/tos.h`, `kernel/`).

Usage:
    python3 scripts/mksdk.py --out build/sdk --toscc build/toscc.exe \
        [--tosput build/tosput.exe] [--version v0.5.0-pre] \
        [--archive dist/tinyos-sdk-windows-amd64.zip]
"""

import argparse
import os
import shutil
import sys
import tarfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The runtime every program links, in the order build.go uses.  thread.c /
# thread_switch.S / kthread.c are only pulled in when a program asks for them
# (they are behind TOS_THREADS / TOS_KTHREADS in lib.h).
RUNTIME_C = [
    "lib.c", "syscall.c", "heap.c", "aeabi.c", "fix16.c", "floatprint.c",
    "faeabi.c", "softfloat32.c",
]
RUNTIME_EXTRA = ["thread.c", "kthread.c", "thread_switch.S"]
CRT0 = ["crt0_arm.S", "crt0_rv32.S", "crt0_x86.S"]
USER_HEADERS = ["lib.h", "heap.h", "fix16.h", "softfloat32.h"]

README = '''# TinyOS user-program SDK {version}

Compile a `.tos` for a TinyOS board with toscc, the Go toolchain. No clang,
no cross toolchain, no full checkout.

```
bin/toscc               C -> RV32 / Thumb / x86, links and packs
bin/tosput              send a .tos to a board over its console
include/                stdio.h, string.h, tos.h, tos_abi.h, ...
kernel/types.h          the base types header every include/*.h starts from
user/                   lib.c, syscall.c, the numeric runtimes, crt0, user.ld
arch/<arch>/usys.h      the syscall instruction and register ABI
examples/hello.c
```

## Build a program

Run this from the directory that contains `user/` (toscc finds the tree by
walking up from the source; `-tos DIR` overrides that):

```
bin/toscc -target esp32c3 -o hello.tos examples/hello.c
```

`-target` names the ISA, not the board: `rv32` (default), `esp32c3`,
`rv32nc`, `arm`, `rp2040`, `rp2350`. A program for any RP2350 board uses
`rp2350`; every board of one ISA shares the binary.

Other flags: `-D NAME[=VAL]`, `-I DIR`, `-o FILE`, `-v`, `-S` (assembly),
`-ir` / `-E` (diagnostics), `-pack` (wrap an ELF that was linked elsewhere),
`-no-build-defines` (ignore `build/build-config*.txt`; there is none in the
SDK, so this is the normal case).

## Send it to the board

```
bin/tosput COM4 hello.tos /bin/hello
```

then run `hello` in the shell. The release images have the console command
enabled; `tosput` only needs the serial port.

The output is an LZ4TOS envelope: a header with stack/heap hints plus the
compressed PIE ELF. The kernel also accepts a raw PIE ELF, which is what a
full `tos build` leaves in `build/user/bin/`.
'''


def copy(src, dst):
    if not os.path.exists(src):
        sys.exit("missing: %s" % src)
    shutil.copy2(src, dst)


def write(path, text):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True, help="SDK directory to create")
    ap.add_argument("--toscc", required=True, help="toscc binary for this platform")
    ap.add_argument("--tosput", default="", help="optional tosput binary")
    ap.add_argument("--version", default="dev")
    ap.add_argument("--archive", default="", help="optional .zip/.tar.gz to write")
    args = ap.parse_args()

    out = os.path.abspath(args.out)
    if os.path.isdir(out):
        shutil.rmtree(out)
    for d in ("bin", "include", "kernel", "user", "arch/rv32", "arch/arm",
              "arch/x86", "examples"):
        os.makedirs(os.path.join(out, d), exist_ok=True)

    for fn in RUNTIME_C + RUNTIME_EXTRA + CRT0 + USER_HEADERS + ["user.ld"]:
        copy(os.path.join(ROOT, "user", fn), os.path.join(out, "user", fn))
    copy(os.path.join(ROOT, "kernel", "types.h"),
         os.path.join(out, "kernel", "types.h"))
    for fn in sorted(os.listdir(os.path.join(ROOT, "include"))):
        if fn.endswith(".h"):
            copy(os.path.join(ROOT, "include", fn),
                 os.path.join(out, "include", fn))
    for arch in ("rv32", "arm", "x86"):
        copy(os.path.join(ROOT, "arch", arch, "usys.h"),
             os.path.join(out, "arch", arch, "usys.h"))

    copy(os.path.join(ROOT, "user", "hello.c"),
         os.path.join(out, "examples", "hello.c"))

    cc = os.path.join(out, "bin", os.path.basename(args.toscc))
    copy(args.toscc, cc)
    os.chmod(cc, 0o755)
    if args.tosput:
        dst = os.path.join(out, "bin", os.path.basename(args.tosput))
        copy(args.tosput, dst)
        os.chmod(dst, 0o755)

    write(os.path.join(out, "README.md"), README.replace("{version}", args.version))

    if args.archive:
        arc = os.path.abspath(args.archive)
        os.makedirs(os.path.dirname(arc) or ".", exist_ok=True)
        if arc.endswith(".zip"):
            with zipfile.ZipFile(arc, "w", zipfile.ZIP_DEFLATED) as z:
                for base, _, files in os.walk(out):
                    for f in files:
                        full = os.path.join(base, f)
                        z.write(full, os.path.join("tinyos-sdk",
                                                   os.path.relpath(full, out)))
        else:
            with tarfile.open(arc, "w:gz") as t:
                t.add(out, arcname="tinyos-sdk")
        print("archive: %s" % arc)
    print("sdk: %s" % out)


if __name__ == "__main__":
    main()
