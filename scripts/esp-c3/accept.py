#!/usr/bin/env python3
"""Real-board acceptance test for the esp32c3 target.

Everything here needs the chip: PMP fault isolation, the flash block device,
preemption, the console line hand-over and the control-character signals.
The emulator covers the same ground on qemu-virt, but the C3 is where the
flash, PMP and USB paths differ.

    scripts/esp-c3/accept.py -p COM4            # boot, then run the checks
    scripts/esp-c3/accept.py -p COM4 --no-reset # talk to a board already up

Each check is a prompt-send-expect cycle: wait for "tos> ", send a line, read
until the next prompt.  A check fails when the expected text is missing or a
panic/trap/fault appears in its output.
"""

import argparse
import re
import sys
import time

import serial

MARK = b"tos> "
# Only kernel-level death is unconditionally bad; a user task trapping is the
# point of half these checks, so those expect a specific fault instead.
BAD = ("KERNEL PANIC", "System halted")


class Board:
    def __init__(self, port, baud=115200):
        self.s = serial.Serial(port, baud, timeout=0.05)
        self.s.dtr = False
        self.s.rts = False

    def reset(self, settle=0.2):
        self.s.rts = True
        time.sleep(0.15)
        self.s.reset_input_buffer()
        self.s.rts = False
        time.sleep(settle)

    def close(self):
        self.s.close()

    def read_until_prompt(self, timeout=15.0):
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.s.read(512)
            if chunk:
                buf += chunk
                if buf.rstrip().endswith(MARK):
                    break
        return buf.decode("utf-8", "replace")

    def boot(self, timeout=25.0):
        self.reset()
        return self.read_until_prompt(timeout)

    def cmd(self, line, timeout=15.0):
        self.s.write(line.encode() + b"\r\n")
        return self.read_until_prompt(timeout)

    def ctrl(self, ch, timeout=4.0):
        self.s.write(bytes([ch]))
        return self.read_until_prompt(timeout)


def check(results, name, out, want=(), avoid=(), timeout_note=""):
    text = out if isinstance(out, str) else out.decode("utf-8", "replace")
    bad = [m for m in BAD if m in text]
    missing = [w for w in want if w not in text]
    ok = not bad and not missing
    results.append((name, ok, text, missing, bad, timeout_note))
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="COM4")
    ap.add_argument("--no-reset", action="store_true")
    args = ap.parse_args()

    b = Board(args.port)
    results = []

    boot = b.boot() if not args.no_reset else b.read_until_prompt(3.0)
    # The file count is not asserted: the flash filesystem keeps whatever an
    # earlier run wrote, so a board that has been used holds more than the 14
    # files the build packs.
    check(results, "boot: banner + filesystem + prompt", boot,
          want=("TinyOS - a tiny multi-platform operating system",
                "littlefs:", "tick self-check", "TinyOS shell"))
    for name, line, want in [
        ("ls lists the programs", "ls", ("bin/sh", "readme.txt")),
        ("hello runs", "hello", ("hello",)),
        ("hello_printf runs", "hello_printf", ()),
        ("mem reports", "mem", ()),
        ("uptime reports", "uptime", ("uptime:", "pid=")),
        ("ps has a cpu-time column", "ps", ("cpu", "sh")),
        ("prot rodata: read-only band is not writable", "prot rodata",
         ("store access fault",)),
        ("prot heap: past the break is unclaimed", "prot heap",
         ("store access fault",)),
        ("prot stack: under the stack is the guard hole", "prot stack",
         ("store access fault",)),
        ("prot null: address zero is unmapped", "prot null",
         ("load access fault",)),
        ("crash: an illegal instruction only kills its task", "crash",
         ("illegal instruction", "tos>")),
        ("shell survives everything above", "", ("tos>",)),
    ]:
        out = b.cmd(line)
        check(results, name, out, want=want)

    # Writes must reach SPI flash and survive a power cycle, which means the
    # littlefs image on the chip and not just a cache.
    check(results, "wfile writes", b.cmd("wfile accept.txt abracadabra"),
          want=("accept.txt",))
    check(results, "cat reads it back", b.cmd("cat accept.txt"),
          want=("abracadabra",))

    # ^C must reach the task blocked on the console.  "cat" with no argument
    # waits for input, so it is the reliable way to have somebody to signal.
    b.s.write(b"cat\r\n")
    time.sleep(0.6)
    out = b.ctrl(0x03)
    check(results, "^C kills the task blocked on the console", out, want=("tos>",))

    after_reset = b.boot()
    check(results, "the write survived a reset", after_reset, want=("TinyOS shell",))
    check(results, "the file is still there after a reset", b.cmd("cat accept.txt"),
          want=("abracadabra",))

    b.close()

    width = max(len(r[0]) for r in results)
    failed = 0
    for name, ok, text, missing, bad, _ in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name:<{width}}")
        if not ok:
            failed += 1
            for m in missing:
                print(f"        missing: {m!r}")
            for m in bad:
                print(f"        unexpected: {m!r}")
            print("        --- output ---")
            for line in text.splitlines()[-12:]:
                print("        " + line)
    print(f"\n{len(results) - failed}/{len(results)} passed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
