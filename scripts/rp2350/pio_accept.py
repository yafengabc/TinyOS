#!/usr/bin/env python3
"""PIO acceptance on an RP2350 (the board that is actually plugged in).

PIO has never run on hardware (OPEN-ITEMS 32).  The RP2040 path has no
automated flow yet, so this drives the RP2350 first - same HAL, three blocks
instead of two, and one extra thing to get wrong (block 2's FUNCSEL and the
pad ISO bit).

What can be checked without eyes or wires is checked exactly:

  * the assembler's output words against a known-good table - for `loopback`
    and `ws2812` those come from the documented expectations (ws2812 was
    cross-checked against pico-sdk's pioasm), for `blink` they are derived
    from the instruction encodings and land on the same `set pindirs,1` word
    that loopback proves;
  * the loopback transfer itself: four words out, four words back, bit for
    bit.  GP4 is both OUT and IN, so this needs no jumper.

The two checks that need eyes (`blink`, `ws2812`) only run with --visual,
which prints the expectation and asks; without it they are reported as
"assembled and loaded", which is what a script can honestly claim.

    scripts/rp2350/pio_accept.py --image rp2350 --uf2 build/rp2350.uf2
    scripts/rp2350/pio_accept.py --image rp2350rvsmp --no-flash
    scripts/rp2350/pio_accept.py --image rp2350 --no-flash --visual --strip

This board's own LED is GP3, not the Pico's GP25, so the blink check defaults
to /pio/blink3.pio and --led-pin 25 switches back to the Pico program.
"""

import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flash as fw                                   # noqa: E402  (BOOTSEL helpers)

import serial                                        # noqa: E402

BAD = ("KERNEL PANIC", "System halted", "MSTKERR", "HardFault")

# Expected instruction words.  `blink` and `blink3` differ only in the .pin
# directive, which is not an instruction, so they share a table.
#
# Index 5 and 10 are `jmp x--, <label>` with no delay, so their delay field is
# 0: 0x0043 / 0x0048.  An earlier revision of this table carried 0x1f43 /
# 0x1f48 (delay 31 copied from the `jmp y--` lines above them), which no
# correct assembler produces - it made a good image look broken.
WORDS = {
    "loopback": [0xE081, 0x80A0, 0x6001, 0x4001, 0x8020],
    "blink": [0xE081, 0xE001, 0xE03F, 0xE05F, 0x1F84, 0x0043,
              0xE000, 0xE03F, 0xE05F, 0x1F89, 0x0048],
    "ws2812": [0x6221, 0x1123, 0x1400, 0xA442],
}

# What the loopback SM hands back after `pio tx 0 0 1 0 1 1`: the same four
# bits, in the same order, one bit per word.  A one-bit `in pins` shifts into
# the top of the ISR (IN_SHIFTDIR = right, which is the reset value), so a
# high level arrives as 0x80000000 and not as 0x00000001.  Measured on the
# RP2350 once `out pins` was finally emitting the LSB: tx 1 0 1 1 gave
# 80000000 00000000 80000000 80000000, and tx 0 1 0 1 gave the matching
# 00000000 80000000 00000000 80000000.  There is no one-cycle lag: the
# synchronised pin is already the level `out pins` drove in the same pass.
RX_WORDS = [0x80000000, 0x00000000, 0x80000000, 0x80000000]

WORD_LINE = re.compile(r"^\s+([0-9a-fA-F]{4}): ([0-9a-fA-F]{4})\s*$", re.M)
RX_LINE = re.compile(r"pio: read (\d+) word\(s\):((?:\s+[0-9a-fA-F]+)+)")


class Console:
    def __init__(self, port, baud=115200, dtr=False):
        self.s = serial.Serial(port, baud, timeout=0.05)
        self.s.dtr = bool(dtr)
        self.log = bytearray()

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def read(self):
        chunk = self.s.read(4096)
        if chunk:
            self.log += chunk
        return chunk

    def pump(self, seconds):
        end = time.time() + seconds
        while time.time() < end:
            self.read()

    def prompt(self, timeout=20.0):
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.read()
            if chunk:
                buf += chunk
                if buf.decode("utf-8", "replace").rstrip().endswith("tos>"):
                    break
        return buf.decode("utf-8", "replace")

    def cmd(self, line, timeout=20.0):
        self.s.write(line.encode() + b"\r\n")
        return self.prompt(timeout)


def check(results, name, text, want=(), avoid=BAD, note=""):
    text = text if isinstance(text, str) else text.decode("utf-8", "replace")
    missing = [w for w in want if w not in text]
    bad = [a for a in avoid if a in text]
    results.append((name, not missing and not bad, text, missing, bad, note))
    return not missing and not bad


def check_ok(results, name, ok, note=""):
    results.append((name, bool(ok), "", [] if ok else ["condition"], [], note))
    return bool(ok)


def words_of(text):
    return [int(m.group(2), 16) for m in WORD_LINE.finditer(text)]


def asm(con, results, path, block, sm, table, pins=None):
    out = con.cmd("pio asm %s %d %d" % (path, block, sm))
    got = words_of(out)
    diff = next((i for i in range(min(len(got), len(table)))
                 if got[i] != table[i]), None)
    ok = got == table
    note = "%d word(s)" % len(got)
    if not ok:
        if diff is None:
            note += ", %d expected" % len(table)
        else:
            note += ", first difference at %d: %04x vs %04x" % (
                diff, got[diff], table[diff])
    check_ok(results, "asm %s -> pio%d sm%d" % (os.path.basename(path),
                                                block, sm), ok, note)
    if pins is not None:
        # The tool prints the window as `gp<base>..<base+count-1>`, so a
        # one-pin window on GP4 reads `gp4..4`, not `gp4..gp4`.
        check_ok(results, "asm %s pin window gp%d..%d"
                 % (os.path.basename(path), pins[0], pins[1]),
                 ("gp%d..%d" % pins) in out, "")
    check(results, "asm %s loaded" % os.path.basename(path), out,
          want=("pio: loaded into",))
    return out


def run_checks(con, results, opts):
    # 1. The files the build packs.  blink3 only exists once this board's LED
    #    pin was noticed, so an old image failing this is information.
    out = con.cmd("pio")
    check(results, "A pio command exists", out, want=("usage: pio asm",))
    out = con.cmd("ls /pio")
    check(results, "A /pio programs present", out,
          want=("loopback.pio", "ws2812.pio", "blink"))

    # 2. Loopback: exact words, then four words out and four back.
    asm(con, results, "/pio/loopback.pio", 0, 0, WORDS["loopback"], (4, 4))
    check(results, "B start", con.cmd("pio start 0 0"), want=("pio: ok",))
    check(results, "B tx 4 words", con.cmd("pio tx 0 0 1 0 1 1"),
          want=("pio: wrote 4",))
    out = con.cmd("pio rx 0 0 4")
    m = RX_LINE.search(out)
    got = [int(x, 16) for x in m.group(2).split()] if m else []
    check_ok(results, "B rx reads the same four words", got == RX_WORDS,
             " ".join("%08x" % w for w in got) or "no rx line")
    check(results, "B stop", con.cmd("pio stop 0 0"), want=("pio: ok",))
    check(results, "B release", con.cmd("pio rel 0 0"), want=("pio: ok",))

    # 3. Blink on this board's own LED (GP3) or the Pico's GP25.
    blink = "/pio/blink3.pio" if opts.led_pin == 3 else "/pio/blink.pio"
    asm(con, results, blink, 1, 1, WORDS["blink"], (opts.led_pin,) * 2)

    # 4. WS2812: block 2 exercises the RP2350-only third block (FUNCSEL 8,
    #    pad ISO).  Assembly is the part a script can judge.
    asm(con, results, "/pio/ws2812.pio", 2, 2, WORDS["ws2812"], (4, 4))

    if opts.visual:
        check(results, "C blink started", con.cmd("pio start 1 1"),
              want=("pio: ok",))
        ans = input("  is the LED on GP%d blinking at about 2 Hz? [y/N] "
                    % opts.led_pin).strip().lower()
        check_ok(results, "C blink seen by eye", ans.startswith("y"), ans)
        con.cmd("pio stop 1 1")
        con.cmd("pio rel 1 1")

        if opts.strip:
            check(results, "D ws2812 started", con.cmd("pio start 2 2"),
                  want=("pio: ok",))
            out = con.cmd("pio tx 2 2 0x00ff0000")
            check(results, "D ws2812 pixel queued", out, want=("wrote",))
            ans = input("  is the strip's first pixel red? [y/N] ").strip().lower()
            check_ok(results, "D ws2812 seen by eye", ans.startswith("y"), ans)
            con.cmd("pio stop 2 2")
            con.cmd("pio rel 2 2")

        check(results, "E all SMs released", con.cmd("pio rel 0 0"),
              want=("pio: ok",))
        out = con.cmd("ls /bin | wc")
        check_ok(results, "E console still healthy after PIO",
                 bool(re.search(r"\d+ \d+ \d+", out)), "wc gave no counts")


def replay_checks(text, opts, results):
    """Judge a saved log with the same expectations (no board needed).

    The board echoes what we type, so the log splits into command blocks on
    `pio asm /pio/<name>.pio <block> <sm>`; each block's word table is
    compared the same way the live path compares it.
    """
    check(results, "A pio command exists", text, want=("usage: pio asm",))
    check(results, "A /pio programs present", text,
          want=("loopback.pio", "ws2812.pio", "blink"))

    seen = 0
    for m in re.finditer(r"pio asm /pio/(\w+)\.pio (\d) (\d)", text):
        name, block, sm = m.group(1), int(m.group(2)), int(m.group(3))
        table = WORDS.get("blink" if name == "blink3" else name)
        if table is None:
            continue
        rest = text[m.end():]
        end = rest.find("tos>")
        got = words_of(rest[:end if end >= 0 else len(rest)])
        diff = next((i for i in range(min(len(got), len(table)))
                     if got[i] != table[i]), None)
        check_ok(results, "asm %s -> pio%d sm%d" % (name, block, sm),
                 got == table,
                 "%d word(s)%s" % (len(got), "" if got == table else
                                   (", first difference at %d: %04x vs %04x"
                                    % (diff, got[diff], table[diff]))
                                   if diff is not None else ""))
        seen += 1
    check_ok(results, "asm tables in the log", seen >= 3,
             "%d asm block(s)" % seen)

    m = RX_LINE.search(text)
    got = [int(x, 16) for x in m.group(2).split()] if m else []
    check_ok(results, "B rx reads the same four words", got == RX_WORDS,
             " ".join("%08x" % w for w in got) or "no rx line")
    check_ok(results, "B loaded into a block",
             "pio: loaded into" in text, "")


def report(results, image):
    print("\nimage %s" % image)
    for name, ok, _text, missing, bad, note in results:
        line = "  %s  %s" % ("PASS" if ok else "FAIL", name)
        if note:
            line += "  (%s)" % note
        print(line)
        if not ok:
            if missing:
                print("        missing: %s" % ", ".join(x for x in missing if x))
            if bad:
                print("        forbidden: %s" % ", ".join(bad))
    passed = sum(1 for r in results if r[1])
    print("\n%d/%d checks passed" % (passed, len(results)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True, choices=[
        "rp2350", "rp2350smp", "rp2350rv", "rp2350rvsmp"])
    ap.add_argument("-p", "--port", default="COM14")
    ap.add_argument("--dtr", action="store_true", help="assert DTR (CDC console)")
    ap.add_argument("--uf2", help="flash this image first")
    ap.add_argument("--no-flash", action="store_true")
    ap.add_argument("--first-boot", type=float, default=25.0)
    ap.add_argument("--led-pin", type=int, default=3,
                    help="3 = this board's LED (blink3.pio), 25 = Pico (blink.pio)")
    ap.add_argument("--visual", action="store_true",
                    help="also ask about the LED and the strip")
    ap.add_argument("--strip", action="store_true",
                    help="a WS2812 strip is attached to GP4")
    ap.add_argument("--log")
    ap.add_argument("--replay", help="judge a saved console log instead")
    args = ap.parse_args()

    if args.replay:
        with open(args.replay, encoding="utf-8", errors="replace") as f:
            text = f.read()
        results = []
        replay_checks(text, args, results)
        report(results, args.image + " (replay)")
        return 0 if all(r[1] for r in results) else 1

    try:
        con = Console(args.port, dtr=args.dtr)
    except serial.SerialException as e:
        print("cannot open %s: %s" % (args.port, e))
        return 2

    results = []
    try:
        if args.uf2 and not args.no_flash:
            print("flashing %s with the console already open ..." % args.uf2)
            root = fw.wait_drive(30, tick=lambda: con.pump(0.05))
            if not root:
                print("no BOOTSEL drive: hold BOOTSEL and replug, or --no-flash")
                return 2
            fw.flash(args.uf2, root, tick=lambda: con.pump(0.05))
            boot = con.prompt(args.first_boot)
        else:
            boot = con.prompt(args.first_boot)
        check_ok(results, "0 boot to a shell", "tos>" in boot,
                 "no prompt in %.0fs" % args.first_boot)

        run_checks(con, results, args)

        log = args.log or os.path.join(
            "build", "sizeprobe",
            "pio-%s-%s.log" % (args.image, time.strftime("%m%d-%H%M%S")))
        try:
            os.makedirs(os.path.dirname(log), exist_ok=True)
            with open(log, "wb") as f:
                f.write(con.log)
            print("\nraw console log: %s" % log)
        except OSError as e:
            print("\ncould not save the log: %s" % e)
    finally:
        con.close()

    report(results, args.image)
    return 0 if all(r[1] for r in results) else 1


if __name__ == "__main__":
    sys.exit(main())
