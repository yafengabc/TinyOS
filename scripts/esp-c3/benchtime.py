#!/usr/bin/env python3
"""Measure guest `bench` on RV32EMU: total host wall time + guest tick counts.

RV32EMU is a pure interpreter whose `uptime()` is an instruction-count tick, so
single-phase wall time is below our 0.2s poll floor.  We report (a) the total
host wall time from sending `bench` to receiving `done`, and (b) the guest's
own tick deltas per phase, which scale with instruction count.
"""
import os, queue, subprocess, sys, threading, time, re

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from emu_path import find_emu  # noqa: E402

EMU = find_emu()
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ELF = os.path.join(ROOT, "build", "kernel-esp32c3.elf")
RD = os.path.join(ROOT, "build", "ramdisk-esp32c3.bin")

def run(bench_args=""):
    if not os.path.exists(EMU):
        return None, "RV32EMU-C not found (run 'python scripts/emu.py build')"
    cmd = [EMU, "-elf", ELF, "-ramdisk", RD,
           "-rxdelay", "5000000", "-rxrate", "64", "-rxperiod", "20000"]
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, bufsize=0)
    q = queue.Queue()
    def pump():
        while True:
            try: d = os.read(p.stdout.fileno(), 65536)
            except OSError: d = b""
            if not d: q.put(b""); return
            q.put(d)
    threading.Thread(target=pump, daemon=True).start()
    buf = bytearray(); t0 = time.time()
    while time.time() - t0 < 30:
        try: d = q.get(timeout=0.2)
        except queue.Empty: d = b""
        if d: buf += d
        if b"tos> " in buf: break
    if b"tos> " not in buf:
        p.kill(); return None, "no prompt"
    line = ("bench " + bench_args).strip() + "\r\n"
    t_send = time.perf_counter()
    p.stdin.write(line.encode()); p.stdin.flush()
    ticks = {}
    t_done = None
    t1 = time.time()
    while time.time() - t1 < 40:
        try: d = q.get(timeout=0.2)
        except queue.Empty: d = b""
        if d: buf += d
        text = buf.decode("utf-8", "replace")
        for ln in text.splitlines():
            ln = ln.strip()
            m = re.match(r"END (\S+) ticks=(-?\d+)", ln)
            if m: ticks[m.group(1)] = int(m.group(2))
            if ln == "done":
                t_done = time.perf_counter(); break
        if t_done: break
    p.kill()
    if t_done is None:
        return None, "no done"
    return (t_done - t_send) * 1000, ticks

def main():
    print("== default 4096 B x 64 (256 KiB per phase) ==")
    for i in range(3):
        wall, ticks = run("")
        if ticks is None:
            print("  run%d: %s" % (i+1, wall)); continue
        print("  run%d: total %7.2f ms  ticks a1=%d b-word=%d a2=%d" %
              (i+1, wall, ticks.get("a1-byte",-1), ticks.get("b-word",-1), ticks.get("a2-byte",-1)))
    print("== scaling: 1024 B x 4 (4 KiB per phase) ==")
    wall, ticks = run("1024 4")
    if ticks is not None:
        print("  total %7.2f ms  ticks a1=%d b-word=%d a2=%d" %
              (wall, ticks.get("a1-byte",-1), ticks.get("b-word",-1), ticks.get("a2-byte",-1)))
    print("(1 tick ~= 1000 emulated instructions under RunHot; 256KiB/4KiB = 64x)")

if __name__ == "__main__":
    main()
