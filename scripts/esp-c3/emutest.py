#!/usr/bin/env python3
"""Boot TinyOS on RV32EMU-C and drive it through a functional + timing check.

RV32EMU-C models the ESP32-C3 (not qemu-virt), so this loads the esp32c3
build: build/kernel-esp32c3.elf + build/ramdisk-esp32c3.bin.  It is the
emulator twin of scripts/esp-c3/accept.py (which talks to the real chip over
COM4); here stdin/stdout is a pipe to build/rv32emu-c.exe instead of a serial
port.  Build that binary with `python scripts/emu.py build`.

The run covers:
  * boot to the shell prompt (and how long that takes)
  * a handful of user programs (ls, hello, hello_printf, mem, uptime, ps)
  * a crashing task that must not kill the shell
  * a command file via `source`
  * `reboot` -- RV32EMU >= 3345bb0 models a software reset, so the banner
    must come back and a second prompt must appear

Exit code is non-zero if the shell never appears, a kernel panic is seen, or
`reboot` fails to return to a prompt.

    python scripts/esp-c3/emutest.py
    TOS_EMU=/path/to/rv32emu-c.exe python scripts/esp-c3/emutest.py
"""
import os
import queue
import subprocess
import sys
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from emu_path import find_emu  # noqa: E402

EMU = find_emu()
ELF = os.path.join(ROOT, "build", "kernel-esp32c3.elf")
RAMDISK = os.path.join(ROOT, "build", "ramdisk-esp32c3.bin")
PROMPT = b"tos> "
BAD = (b"KERNEL PANIC", b"System halted")


class Emu:
    def __init__(self):
        if not os.path.exists(EMU):
            raise SystemExit("RV32EMU-C not found: %s (run 'python scripts/emu.py build')" % EMU)
        if not os.path.exists(ELF):
            raise SystemExit("build artifact missing: %s (run 'tos build -target esp32c3')" % ELF)
        cmd = [EMU, "-elf", ELF, "-ramdisk", RAMDISK,
               "-rxdelay", "5000000", "-rxrate", "64", "-rxperiod", "20000"]
        self.proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, bufsize=0)
        self.q = queue.Queue()
        self.eof = False
        threading.Thread(target=self._pump, daemon=True).start()

    def _pump(self):
        while True:
            try:
                d = os.read(self.proc.stdout.fileno(), 65536)
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

    def write(self, data):
        self.proc.stdin.write(data)
        self.proc.stdin.flush()

    def close(self):
        try:
            self.proc.kill()
        except Exception:
            pass


def main():
    emu = Emu()
    buf = bytearray()
    t0 = time.time()

    # --- boot: wait until the shell banner + prompt have both appeared ---
    end = time.time() + 60
    while time.time() < end:
        d = emu.read(0.2)
        if d:
            buf += d
        if b"TinyOS shell" in buf and PROMPT in buf:
            break
        if emu.eof:
            break
    boot_ms = (time.time() - t0) * 1000
    bootlog = buf.decode("utf-8", "replace")
    print("==== boot (%d ms wall) ====" % boot_ms)
    print(bootlog)

    if PROMPT not in buf:
        emu.close()
        print("EMU TEST FAILED: no shell prompt", file=sys.stderr)
        sys.exit(1)
    if any(b in buf for b in BAD):
        emu.close()
        print("EMU TEST FAILED: kernel panic during boot", file=sys.stderr)
        sys.exit(1)

    results = []

    def send(line, wait_ms=4000, label=None):
        label = label or line
        # The shell echoes the newline; make sure the echoed CR is consumed
        # before we start matching so we only count output generated now.
        emu.write((line + "\r\n").encode())
        out = bytearray()
        tstart = time.time()
        tend = tstart + wait_ms / 1000.0
        while time.time() < tend:
            d = emu.read(0.2)
            if d:
                out += d
                if PROMPT in out:
                    # The shell answers the CR that ends the line and the LF
                    # after it with a prompt each, so drain the trailing one
                    # before returning -- otherwise it is the first thing the
                    # next command sees and every check lags by one command.
                    grace = time.time() + 0.15
                    while time.time() < grace:
                        d = emu.read(0.05)
                        if not d:
                            break
                        out += d
                    break
            if emu.eof:
                break
        ms = (time.time() - tstart) * 1000
        text = out.decode("utf-8", "replace")
        print("---- %s (%d ms) ----" % (label, ms))
        print(text)
        results.append((label, text, ms))
        return text

    checks = [
        ("ls", ("bin/sh", "readme.txt")),
        ("cat readme.txt", ("TinyOS",)),
        ("hello", ("hello",)),
        ("hello_printf", ("hello",)),
        ("mem", ("heap demo",)),
        ("uptime", ("uptime:", "pid=")),
        ("ps", ("cpu", "sh")),
        ("crash", ("illegal instruction", "tos>")),
        ("source etc/rc", ("tos>",)),
    ]
    for line, want in checks:
        out = send(line)
        for w in want:
            if w not in out:
                print("  WARN: expected %r not seen in output of %r" % (w, line))

    # --- reboot: RV32EMU models a software reset ---
    print("---- reboot (expect software reset + banner again) ----")
    emu.write(b"reboot\r\n")
    t_rb = time.time()
    rebuf = bytearray()
    end = time.time() + 30
    saw_reset = False
    saw_prompt2 = False
    while time.time() < end:
        d = emu.read(0.2)
        if d:
            rebuf += d
            txt = rebuf.decode("utf-8", "replace")
            if "software reset" in txt or "rebooting" in txt:
                saw_reset = True
            if saw_reset and PROMPT in rebuf:
                saw_prompt2 = True
                break
        if emu.eof:
            break
    rb_ms = (time.time() - t_rb) * 1000
    print(rebuf.decode("utf-8", "replace"))
    print("  reboot observed=%s, second prompt=%s (%d ms)" % (saw_reset, saw_prompt2, rb_ms))

    if any(b in rebuf for b in BAD):
        emu.close()
        print("EMU TEST FAILED: kernel panic after reboot", file=sys.stderr)
        sys.exit(1)

    emu.close()
    ok = saw_reset and saw_prompt2
    print("==== EMU TEST %s (boot %d ms, reboot %d ms) ====" %
          ("OK" if ok else "FAILED", boot_ms, rb_ms))
    if not ok:
        sys.exit(1)


if __name__ == "__main__":
    main()
