#!/usr/bin/env python3
"""Real-board acceptance test for the esp32c3-generic target.

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
import os
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
                if buf.endswith(MARK):
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


def check_ok(results, name, ok, note=""):
    """For a condition the caller already computed (a line-by-line compare)."""
    results.append((name, bool(ok), "", [] if ok else ["condition"], [], note))
    return bool(ok)


def output_lines(text, echo):
    """The command's own output: drop the line the tty echoed, the prompt, and
    rvm's timing trailer."""
    lines = [l.strip() for l in text.replace("\r", "").splitlines()]
    if lines and lines[0] == echo.strip():
        lines = lines[1:]
    return [l for l in lines
            if l and not l.startswith("tos>") and not l.startswith("result=")]


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
    # files the build packs.  The mount itself is covered by "ls" below - the
    # banner stopped printing a "littlefs:" line when the boot log was
    # trimmed, and this check kept looking for it.
    check(results, "boot: banner + filesystem + prompt", boot,
          want=("TinyOS - a tiny multi-platform operating system",
                "arena", "tick self-check", "TinyOS shell"))
    for name, line, want in [
        # `ls` lists one level: a directory shows as "bin/", not "bin/sh".
        ("ls lists the programs", "ls", ("bin/", "readme.txt")),
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

    # exec() path rules: a name with a slash is resolved against the cwd (so
    # ./prog works), and a bare name looks in the cwd first, then /bin.  Copy
    # a known program into the cwd and run it both ways; the bare name must
    # find the copy, not the /bin original.
    b.cmd("cat /bin/hello > acc_h")
    check(results, "exec: ./name runs from the cwd", b.cmd("./acc_h"),
          want=("hello from ring 3!",))
    check(results, "exec: bare name finds the cwd copy", b.cmd("acc_h"),
          want=("hello from ring 3!",))
    check(results, "the cwd copy can be removed", b.cmd("rm acc_h"),
          want=("tos>",))

    # Command files: the same parser with a file as its input source.  `source`
    # runs in this process on purpose - it costs no window at all, while a
    # shell that execs a script which execs a program spends one window per
    # level (the C3 has 192 KiB of them, so twelve 16 KiB windows, which is
    # plenty - but nesting is still how a recursive script eats the board).
    # The script is written by wfile so the image needs no extra entry.
    check(results, "wfile writes a script", b.cmd("wfile s.sh hello"),
          want=("s.sh",))
    check(results, "source runs the command file", b.cmd("source s.sh"),
          want=("hello from ring 3!",))
    check(results, "source names the file it cannot open", b.cmd("source nosuch"),
          want=("sh: cannot open nosuch",))
    check(results, "the boot script parses", b.cmd("run sh etc/rc"),
          want=("tos>",))

    # The on-board toolchain, end to end: csc compiles the script that ships in
    # the image (/tests/cover.c), rvm interprets the RIR, rcc compiles it to a
    # native image, and both must print tests/onboard/cover.expected.  This is
    # the same script scripts/rvm-cover.py runs on QEMU; the C3 is the board
    # half of the coverage.
    exp_path = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), "tests", "onboard", "cover.expected")
    exp = [l.strip() for l in open(exp_path, encoding="utf-8") if l.strip()]

    out = b.cmd("csc /tests/cover.c /tests/cover.rir", timeout=60.0)
    check(results, "csc compiles /tests/cover.c", out,
          want=("csc: /tests/cover.c",))

    out = b.cmd("rvm /tests/cover.rir", timeout=30.0)
    rvm_lines = output_lines(out, "rvm /tests/cover.rir")
    check_ok(results, "rvm cover output matches cover.expected", rvm_lines == exp,
             "%d/%d lines" % (len(rvm_lines), len(exp)))

    out = b.cmd("rcc /tests/cover.rir /bin/cover", timeout=60.0)
    check(results, "rcc compiles the native image", out, want=("rcc:",))

    out = b.cmd("cover", timeout=30.0)
    native_lines = output_lines(out, "cover")
    check_ok(results, "native cover output matches cover.expected",
             native_lines == exp, "%d/%d lines" % (len(native_lines), len(exp)))
    check_ok(results, "rvm and native agree", rvm_lines == native_lines, "")

    # The same for the state-machine demo: csc's per-function ceilings (256
    # AST / 256 IR on this build) are why the split-by-job pattern matters, so
    # the demo ships and is compiled here too.
    exp_path = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), "tests", "onboard", "state.expected")
    exp_state = [l.strip() for l in open(exp_path, encoding="utf-8") if l.strip()]

    out = b.cmd("csc /tests/state.c /tests/state.rir", timeout=60.0)
    check(results, "csc compiles the state-machine demo", out,
          want=("csc: /tests/state.c",))
    out = b.cmd("rvm /tests/state.rir", timeout=30.0)
    rvm_state = output_lines(out, "rvm /tests/state.rir")
    check_ok(results, "rvm state output matches state.expected",
             rvm_state == exp_state,
             "%d/%d lines" % (len(rvm_state), len(exp_state)))
    out = b.cmd("rcc /tests/state.rir /bin/state", timeout=60.0)
    check(results, "rcc compiles the state-machine demo", out, want=("rcc:",))
    out = b.cmd("state", timeout=30.0)
    native_state = output_lines(out, "state")
    check_ok(results, "native state output matches state.expected",
             native_state == exp_state,
             "%d/%d lines" % (len(native_state), len(exp_state)))
    check_ok(results, "state: rvm and native agree", rvm_state == native_state, "")

    # The scheduling natives: ready(fd) is one non-blocking poll, yield() gives
    # up the CPU for a round.  /readme.txt is always in the image, so the
    # descriptor is readable (ready=1) and yield returns 0.
    exp_path = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), "tests", "onboard", "ready.expected")
    exp_ready = [l.strip() for l in open(exp_path, encoding="utf-8") if l.strip()]

    out = b.cmd("csc /tests/ready.c /tests/ready.rir", timeout=60.0)
    check(results, "csc compiles the ready/yield demo", out,
          want=("csc: /tests/ready.c",))
    out = b.cmd("rvm /tests/ready.rir", timeout=30.0)
    rvm_ready = output_lines(out, "rvm /tests/ready.rir")
    check_ok(results, "rvm ready/yield matches ready.expected",
             rvm_ready == exp_ready,
             "%d/%d lines" % (len(rvm_ready), len(exp_ready)))
    out = b.cmd("rcc /tests/ready.rir /bin/ready", timeout=60.0)
    check(results, "rcc compiles the ready/yield demo", out, want=("rcc:",))
    out = b.cmd("ready", timeout=30.0)
    native_ready = output_lines(out, "ready")
    check_ok(results, "native ready/yield matches ready.expected",
             native_ready == exp_ready,
             "%d/%d lines" % (len(native_ready), len(exp_ready)))
    check_ok(results, "ready: rvm and native agree", rvm_ready == native_ready, "")

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

    # `reboot` has to reset the chip itself - no EN pulse from the host, no
    # esptool.  The proof is the ROM's own reset-cause line
    # (rst:0x3 RTC_SW_SYS_RST), then the banner coming back on its own.
    out = b.cmd("reboot", timeout=25.0)
    check(results, "reboot resets the chip", out,
          want=("reboot: resetting the machine", "reboot requested by pid", "RTC_SW_SYS_RST",
                "TinyOS - a tiny multi-platform operating system",
                "TinyOS shell"))
    check(results, "the filesystem survives a reboot", b.cmd("cat accept.txt"),
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
