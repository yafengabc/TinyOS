#!/usr/bin/env python3
"""Run bin/bench in either RV32EMU or QEMU and time it from the host.

Why this exists: QEMU's TCG can decide to re-translate the same tight loop
for every iteration, which makes a copy loop cost microseconds per iteration
instead of nanoseconds - see docs/PLATFORM-DESIGN.md, "同一个现象在 -O2 下会
复发".  That makes QEMU rv32 useless as a performance model, and it is worth
knowing whether our own emulator inherited the problem.

The guest cannot answer "how fast": `uptime()` is wall time on QEMU but an
instruction-count tick on RV32EMU.  So the host timestamps the guest's
START/END marker lines, which is the one clock both machines share.  The
guest's own tick deltas are printed next to it as a cross-check.

    python scripts/bench.py emu                 # RV32EMU, default payload
    python scripts/bench.py qemu 5599           # QEMU on a serial port
    python scripts/bench.py qemu 5599 "bench 4096 8"

Payload is `bench [bytes] [reps]`; the default is 4 KiB x 64 = 256 KiB of
copying per phase, which is ~10 ms on RV32EMU and ~3 s on QEMU.
"""
import os
import queue
import re
import socket
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMU = os.environ.get("TOS_EMU", r"C:\Users\SONG\Documents\Projects\RV32EMU\rv32emu.exe")
QEMU = os.environ.get("TOS_QEMU", r"C:\msys64\ucrt64\bin\qemu-system-riscv32.exe")
DEFAULT_CMD = "bench"


class Source:
    """Byte stream with a reader thread: select() is socket-only on Windows
    and this has to work for both a pipe and a TCP socket."""

    def __init__(self):
        self.q = queue.Queue()
        self.eof = False

    def _pump(self, readfn):
        while True:
            try:
                d = readfn()
            except OSError:
                d = b""
            if not d:
                self.eof = True
                self.q.put(b"")
                return
            self.q.put(d)

    def read(self, timeout):
        try:
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return b""

    def close(self):
        try:
            self.proc.kill()
        except Exception:
            pass


class Pipe(Source):
    def __init__(self, proc):
        super().__init__()
        self.proc = proc

        def rd():
            return os.read(proc.stdout.fileno(), 65536)

        threading.Thread(target=self._pump, args=(rd,), daemon=True).start()

    def write(self, data):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()


class Sock(Source):
    def __init__(self, proc, port):
        super().__init__()
        self.proc = proc
        deadline = time.time() + 20
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)

        def rd():
            return self.s.recv(65536)

        # create_connection() leaves a 2 s timeout behind, and a silent
        # guest is exactly what a slow benchmark has to survive.
        self.s.settimeout(None)
        threading.Thread(target=self._pump, args=(rd,), daemon=True).start()

    def write(self, data):
        self.s.sendall(data)

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass
        super().close()


def start_emu():
    # RV32EMU models the ESP32-C3, not QEMU's virt board, so it gets the C3
    # build.  The user binaries are bit-identical between the two rv32
    # targets (internal/build/build.go: "an esp32c3 user binary is
    # bit-identical to a rv32 one"), which is what makes the comparison
    # meaningful: bin/bench is the same guest instruction stream in both.
    cmd = [EMU,
           "-elf", os.path.join(ROOT, "build", "kernel-esp32c3.elf"),
           "-ramdisk", os.path.join(ROOT, "build", "ramdisk-esp32c3.bin"),
           "-rxdelay", "5000000", "-rxrate", "64", "-rxperiod", "20000"]
    return Pipe(subprocess.Popen(cmd, stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, bufsize=0))


def start_qemu(port):
    p = subprocess.Popen(
        [QEMU, "-machine", "virt", "-m", "256M", "-bios", "none",
         "-kernel", os.path.join(ROOT, "build", "kernel-rv32.elf"),
         "-display", "none",
         "-chardev", "socket,id=tosserial,host=127.0.0.1,port=%d,server=on,wait=on" % port,
         "-serial", "chardev:tosserial", "-monitor", "none"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return Sock(p, port)


def pump(src, until, timeout):
    """Read until until(text) holds; return [(time, line), ...] and the text."""
    stamps = []
    text = ""
    partial = ""
    end = time.time() + timeout
    while time.time() < end:
        d = src.read(0.2)
        if not d:
            if src.eof:
                break
            continue
        chunk = d.decode("utf-8", "replace")
        text += chunk
        partial += chunk
        parts = partial.split("\n")
        partial = parts.pop()
        for raw in parts:
            line = raw.replace("\r", "").strip()
            if line:
                stamps.append((time.time(), line))
        if until(text):
            break
    return stamps, text


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else "emu"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 5599
    cmd = sys.argv[3] if len(sys.argv) > 3 else DEFAULT_CMD
    if mode not in ("emu", "qemu"):
        raise SystemExit(__doc__)

    src = start_emu() if mode == "emu" else start_qemu(port)

    _, bootlog = pump(src, lambda t: "tos> " in t, 300)
    if "tos> " not in bootlog:
        src.close()
        raise SystemExit("no shell prompt from %s: %r" % (mode, bootlog[-300:]))
    time.sleep(0.3)
    src.write((cmd + "\r\n").encode())

    stamps, _ = pump(src, lambda t: re.search(r"^done$", t, re.M) is not None, 900)
    src.close()

    # the payload is printed by the guest, so the host does not have to
    # agree with bench.c about the default sizes
    size, reps = 4096, 64
    for _, line in stamps:
        m = re.match(r"bench: (\d+) B x (\d+) reps", line)
        if m:
            size, reps = int(m.group(1)), int(m.group(2))

    marks = {}
    for t, line in stamps:
        m = re.match(r"START (\S+)$", line)
        if m:
            marks.setdefault(m.group(1), []).append((t, None))
        m = re.match(r"END (\S+) ticks=(-?\d+) sum=(-?\d+)$", line)
        if m:
            marks.setdefault(m.group(1), []).append((t, int(m.group(2))))

    t0 = stamps[0][0] if stamps else 0
    print("---- %s ----" % mode)
    for t, line in stamps:
        print("%8.3f  %s" % (t - t0, line))
    print()
    print("%-9s %10s %13s %8s %10s %10s" %
          ("phase", "wall(ms)", "guest-ticks", "iters", "ns/iter", "ns/byte"))
    for name in ("a1-byte", "b-word", "a2-byte"):
        ms = marks.get(name, [])
        if len(ms) < 2:
            print("%-9s <not measured>" % name)
            continue
        (ts, _), (te, ticks) = ms[0], ms[1]
        iters = size * reps if name.endswith("byte") else size // 4 * reps
        wall = te - ts
        print("%-9s %10.1f %13s %8d %10.1f %10.1f" %
              (name, wall * 1000, ticks, iters,
               wall * 1e9 / iters, wall * 1e9 / (size * reps)))

    # The emulator or QEMU is already gone; a reader thread parked in recv
    # must not hold the interpreter open.
    sys.stdout.flush()
    os._exit(0)


main()
