#!/usr/bin/env python3
"""Build and smoke-test the standard TinyOS runtime (RV32EMU-C).

RV32EMU-C lives in the `rv32emu-c/` submodule. QEMU stays useful for functional
runs (`tos run -target rv32`), but it cannot be used to measure TinyOS: its TCG
re-translates the same TB whenever a store lands on a translated code page, so a
copy loop costs microseconds per iteration instead of nanoseconds (see
docs/PLATFORM-DESIGN.md). The emulator is the performance model, so the test
scripts default to this binary.

    python scripts/emu.py build     # build into build/rv32emu-c.exe
    python scripts/emu.py test      # boot the esp32c3 kernel and poke it
    python scripts/emu.py           # test (builds if missing)

Environment:
    TOS_EMU   overrides the binary used by the scripts (this file only builds
              the local one)
"""
import argparse
import os
import queue
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SUB = os.path.join(ROOT, "rv32emu-c")
EXE_NAME = "rv32emu-c.exe" if os.name == "nt" else "rv32emu-c"
OUT = os.path.join(ROOT, "build", EXE_NAME)
ELF = os.path.join(ROOT, "build", "kernel-esp32c3.elf")
RAMDISK = os.path.join(ROOT, "build", "ramdisk-esp32c3.bin")


def submodule_ready():
    return os.path.isdir(SUB) and os.path.exists(os.path.join(SUB, "src", "main.c"))


def build():
    if not submodule_ready():
        print("rv32emu-c/ submodule is missing; run:\n"
              "  git submodule update --init --recursive", file=sys.stderr)
        return False
    if os.name == "nt":
        script = os.path.join(SUB, "build.ps1")
        cmd = ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", script]
    else:
        cmd = ["make", "-C", SUB]
    # Reuse the compiler paths `tos` already knows about: TOS_BIN entries, then
    # the default MSYS2 UCRT64 bin. build.ps1 itself accepts gcc or clang.
    env = os.environ.copy()
    extra = [p for p in os.environ.get("TOS_BIN", "").split(os.pathsep) if p]
    if os.name == "nt":
        default_bin = r"C:\msys64\ucrt64\bin"
        if os.path.isdir(default_bin):
            extra.append(default_bin)
    if extra:
        env["PATH"] = os.pathsep.join(extra + [env.get("PATH", "")])
    r = subprocess.run(cmd, env=env)
    if r.returncode != 0:
        return False
    src = os.path.join(SUB, "build", EXE_NAME)
    if not os.path.exists(src):
        print("build finished but %s is missing" % src, file=sys.stderr)
        return False
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(src, "rb") as f:
        data = f.read()
    with open(OUT, "wb") as f:
        f.write(data)
    print("ok -> %s (%d bytes)" % (os.path.relpath(OUT, ROOT), len(data)))
    return True


class Emu:
    def __init__(self, path):
        self.proc = subprocess.Popen(
            [path, "-elf", ELF, "-ramdisk", RAMDISK,
             "-rxdelay", "5000000", "-rxrate", "64", "-rxperiod", "20000"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0)
        self.q = queue.Queue()
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        while True:
            try:
                data = os.read(self.proc.stdout.fileno(), 65536)
            except OSError:
                data = b""
            if not data:
                self.q.put(b"")
                return
            self.q.put(data)

    def wait_for(self, needle, timeout):
        deadline = time.perf_counter() + timeout
        buf = b""
        while time.perf_counter() < deadline:
            try:
                data = self.q.get(timeout=0.05)
            except queue.Empty:
                continue
            if not data:
                break
            buf += data
            if needle in buf:
                return True, buf
        return False, buf

    def send(self, line):
        self.proc.stdin.write(line.encode() + b"\r\n")
        self.proc.stdin.flush()

    def close(self):
        self.proc.kill()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            pass


def test():
    if not os.path.exists(OUT):
        print("missing %s: run 'python scripts/emu.py build'" % OUT, file=sys.stderr)
        return False
    for path in (ELF, RAMDISK):
        if not os.path.exists(path):
            print("missing %s: run 'tos build -target esp32c3'" % path, file=sys.stderr)
            return False

    emu = Emu(OUT)
    try:
        t0 = time.perf_counter()
        ok, buf = emu.wait_for(b"tos> ", 10.0)
        boot = time.perf_counter() - t0
        if not ok:
            print("no shell prompt; last output:")
            print(buf[-800:].decode("utf-8", "replace"))
            return False
        print("boot: %.3f s" % boot)

        emu.send("uptime")
        ok, buf = emu.wait_for(b"ms (", 5.0)
        if not ok:
            print("`uptime` produced no answer; last output:")
            print(buf[-800:].decode("utf-8", "replace"))
            return False
        for line in buf.decode("utf-8", "replace").splitlines():
            if "uptime:" in line:
                print("uptime: " + line.strip())
                break

        emu.send("ls /")
        ok, _ = emu.wait_for(b"bin/sh", 5.0)
        if not ok:
            print("`ls` did not list the filesystem")
            return False
        print("filesystem: ok")
    finally:
        emu.close()

    print("SMOKE TEST OK")
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", nargs="?", default="test", choices=["build", "test"])
    args = ap.parse_args()

    if args.action == "build":
        return 0 if build() else 1
    if not os.path.exists(OUT) and not build():
        return 1
    return 0 if test() else 1


if __name__ == "__main__":
    sys.exit(main())
