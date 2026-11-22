#!/usr/bin/env python3
"""Section-33 acceptance run for the RP2350 (ARM and Hazard3 images).

Section 33 of docs/OPEN-ITEMS.md is a list of questions only the chip can
answer: does the RV image speak after a flash without a RESET, does `df` agree
with the partition, do the four `prot` cases fault, does the stack grow, is
the second core still worth 1.8x.  Asking them by hand is how the last round
ended up with three different answers to "which reset path was that?", so this
script asks them in one order and keeps the raw log.

    scripts/rp2350/accept33.py --image atk-dnr2350am-rv32 --uf2 build/tinyos-atk-dnr2350am-rv32.uf2
    scripts/rp2350/accept33.py --image atk-dnr2350am-smp --no-flash
    scripts/rp2350/accept33.py --image atk-dnr2350am-rv32 --replay build/sizeprobe/x.log

The interesting part is the first boot.  --uf2 flashes over the BOOTSEL drive
with the serial port already open, and then *never touches RTS and never
reopens it*: whatever arrives is what the ROM's own reboot produced.  A script
that pulsed RTS or reopened the port would hide the very bug 33.4 is about.

Default port is UART0 (COM14 on this board) because it is a separate
USB-serial chip that stays present while the board is in BOOTSEL; the native
CDC port disappears at exactly the wrong moment and needs --dtr.
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

# Per-image expectations: cpus drives the smpbench check, rv switches the
# Hazard3-only boot marker (the [A] clock snapshot).
IMAGES = {
    "atk-dnr2350am":       dict(cpus=1, rv=False),
    "atk-dnr2350am-smp":   dict(cpus=2, rv=False),
    "atk-dnr2350am-rv32":    dict(cpus=1, rv=True),
    "atk-dnr2350am-rv32-smp": dict(cpus=2, rv=True),
}

SNAPSHOT = re.compile(r"\[A\] pre-pll ok sys=(0x[0-9A-Fa-f]+) "
                      r"ref=(0x[0-9A-Fa-f]+) xosc=(0x[0-9A-Fa-f]+)")


class Console:
    """One serial handle plus an accumulating log, never reset behind our back."""

    def __init__(self, port, baud=115200, dtr=False):
        self.s = serial.Serial(port, baud, timeout=0.05)
        # DTR low reads as "no host": the RP2350 CDC port then drops output.
        # RTS is left alone - on some adapters it is wired to RUN, which is
        # exactly the reset this script exists to avoid.
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

    def wait(self, pred, timeout):
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.read()
            if chunk:
                buf += chunk
                if pred(buf.decode("utf-8", "replace")):
                    break
        return buf.decode("utf-8", "replace")

    def prompt(self, timeout=20.0):
        return self.wait(lambda t: t.rstrip().endswith("tos>"), timeout)

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
    """For a condition the caller already computed (a parsed number, a count)."""
    results.append((name, bool(ok), "", [] if ok else ["condition"], [], note))
    return bool(ok)


def msh_wink(ps_text):
    """`wink` from the msh row: pid ppid prio state c cpu% cpums wink stk/sz name."""
    for line in ps_text.splitlines():
        t = line.split()
        if len(t) >= 4 and t[-1] == "msh":
            return t[-3]
    return None


def wait_boot(con, timeout, start=0):
    """Read until the prompt, timing the first byte and the prompt.

    The bug in 33.4 was not "no output" but "output after 120 s", so the
    latency is part of the result, not a detail: a 20-second boot would
    otherwise pass a check that only asked for output within 25.

    `start` is an offset into the console log.  The flash path pumps the port
    while waiting for the drive to unmount, and on this part the shell can be
    up before that returns (measured 0.35 s), so the [A] line may already be
    in the log before this function is called.  Reading from the log instead
    of from "now" is what keeps the snapshot in the text the checks look at.
    """
    t0 = time.time()
    first = None
    end = t0 + timeout
    while time.time() < end:
        text = bytes(con.log[start:]).decode("utf-8", "replace")
        if text.rstrip().endswith("tos>"):
            break
        chunk = con.read()
        if chunk and first is None:
            first = time.time() - t0
    text = bytes(con.log[start:]).decode("utf-8", "replace")
    if first is None and text:
        # Arrived while flash() was still waiting for the drive to go away.
        first = 0.0
    return text, first, time.time() - t0


def run_checks(con, image, results, opts, boot_text):
    """The body of section 33, in the order the list asks for it."""
    spec = IMAGES[image]

    # --- 33.4: what the ROM handed over, and the clock tree that followed ---
    if spec["rv"]:
        m = SNAPSHOT.search(boot_text)
        check(results, "A 33.4 clock snapshot printed", boot_text,
              want=() if m else ("[A] pre-pll ok",),
              note=("sys=%s ref=%s xosc=%s" % m.groups()) if m else
                   "no snapshot: old image?")
        check(results, "A clock measured", boot_text, want=("[B] clk=0x",))
    else:
        # The ARM port has no [A] pre-pll line - that snapshot exists because
        # the RV32 clock bug needed one.  Its chain starts at [B] clk.
        check(results, "A boot markers [B]..[E]", boot_text,
              want=("[B] clk=", "[C] vtor+sau", "[D] cpu0", "[E] usb"))

    # --- 33.0: 3.5 MiB partition, grown without a warning ---
    check(results, "B no fs-grow warning", boot_text, avoid=("fs grow",))
    check(results, "B df total 3670016", con.cmd("df"), want=("3670016",))

    # --- 33.1/33.2: windows, isolation, stack ---
    wink = msh_wink(con.cmd("ps"))
    check(results, "C ps lists msh", "msh wink %s" % wink if wink else "",
          want=("msh wink",), note=("msh wink %s" % wink) if wink else
          "could not parse ps")

    # RV32 survives two of the four by design, not by oversight: rodata shares
    # its 4 KiB page with .text that must stay executable, and address zero
    # falls in the bootrom-hardwired RWX entries 8..10 which software cannot
    # override.  Both are recorded in OPEN-ITEMS 31.0; what still has to fault
    # there is the heap, the stack and a device register.  The ARM port denies
    # all four because PMSAv8 is byte-granular.
    # The two ports name the same event differently: rv32 prints
    # "load/store access fault", the M33 prints "MemManage".  Matching only
    # one string makes every prot case fail on the other arch.
    fault_mark = "access fault" if spec["rv"] else "MemManage"
    rv_survives = {"prot rodata", "prot null"}
    for line in ("prot rodata", "prot heap", "prot stack", "prot null",
                 "prot dev 0x40070000"):
        out = con.cmd(line)
        if spec["rv"] and line in rv_survives:
            check(results, "D %s (RV32: survives)" % line, out,
                  want=("survived",))
        else:
            check(results, "D " + line, out, want=(fault_mark,))

    out = con.cmd("deep 3", timeout=25.0)
    check(results, "E deep 3 grows the stack", out, want=("stack grown",))
    check(results, "E deep 3 journal intact", out, want=("journal intact",))

    # stk asks for its own 16 KiB stack through TOS_STACK.  On a board whose
    # port puts the stack in its own aligned region (stack_align = 1, the
    # Hazard3/PMP half) that loads only when the hint divides the window
    # grain; a mismatch used to reach the board and fail at exec with
    # "carries a bad stack hint" (issue #44).
    out = con.cmd("stk 3 100", timeout=30.0)
    check(results, "E stk 3 loads a 16 KiB stack hint", out,
          avoid=BAD + ("bad stack hint", "exec:"),
          note="no hint refusal, no fault")

    # --- 33.2: does the second core still earn its keep ---
    if spec["cpus"] > 1:
        out = con.cmd("smpbench", timeout=60.0)
        m = re.search(r"speedup:\s*(\d+)\.(\d+)x", out)
        ratio = (int(m.group(1)) * 100 + int(m.group(2))) if m else 0
        check_ok(results, "F smpbench >= 1.8x", ratio >= 180,
                 ("%.2fx" % (ratio / 100.0)) if m else "no ratio parsed")
        check(results, "F smp -s two workers", con.cmd("smp -s 2 200", 30.0),
              want=("worker 1",))

    # --- 33.4b: one C Script through csc -> rvm -> rcc ---
    # The image carries /tests/cover.c (files/tests/cover.c).  The interpreter
    # and the native image must both print tests/onboard/cover.expected; this
    # is the single-script coverage of the on-board toolchain.
    exp_path = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__)))), "tests", "onboard", "cover.expected")
    exp = [l.strip() for l in open(exp_path, encoding="utf-8") if l.strip()]
    out = con.cmd("csc /tests/cover.c /tests/cover.rir", 60.0)
    check(results, "G csc cover.c", out, want=("csc: /tests/cover.c",))
    out = con.cmd("rvm /tests/cover.rir", 30.0)
    rvm_lines = [l.strip() for l in out.splitlines()
                 if l.strip() and not l.strip().startswith("result=")]
    check_ok(results, "G rvm cover output", rvm_lines == exp,
             "%d/%d lines" % (len(rvm_lines), len(exp)))
    out = con.cmd("rcc /tests/cover.rir /bin/cover", 60.0)
    check(results, "G rcc cover.rir", out, want=("rcc:",))
    out = con.cmd("cover", 30.0)
    nat_lines = [l.strip() for l in out.splitlines() if l.strip()]
    check_ok(results, "G native cover output", nat_lines == exp,
             "%d/%d lines" % (len(nat_lines), len(exp)))
    check_ok(results, "G rvm == native", rvm_lines == nat_lines, "")

    # --- 29.1: the flash write path, optional because it is minutes ---
    if opts.dd_rounds:
        ok = 0
        for i in range(opts.dd_rounds):
            out = con.cmd("dd if=/dev/zero of=/x bs=4096 count=8", 30.0)
            out += con.cmd("dd if=/x bs=4096 count=8", 30.0)
            out += con.cmd("rm /x", 20.0)
            if "pattern verified" in out:
                ok += 1
            if i % 10 == 0:
                print("  dd round %d/%d ..." % (i + 1, opts.dd_rounds),
                      flush=True)
        check(results, "G dd+rm %d rounds" % opts.dd_rounds,
              "%d/%d" % (ok, opts.dd_rounds),
              want=("%d/%d" % (opts.dd_rounds, opts.dd_rounds),))


def replay_checks(text, image, results):
    """The same questions, answered from a saved log (no board needed).

    Useful twice over: the other side can send a log instead of a description,
    and the parsing itself can be exercised without hardware.
    """
    run_checks_replay = IMAGES[image]
    if run_checks_replay["rv"]:
        m = SNAPSHOT.search(text)
        check(results, "A 33.4 clock snapshot in log", text,
              want=() if m else ("[A] pre-pll ok",),
              note=("sys=%s ref=%s xosc=%s" % m.groups()) if m else "")
        check(results, "A clock measured", text, want=("[B] clk=0x",))
    else:
        # ARM images start their banner at [B]; [A] pre-pll is RV32-only.
        check(results, "A boot markers in log", text,
              want=("[B] clk=", "[C] vtor+sau", "[D] cpu0", "[E] usb"))

    check(results, "B no fs-grow warning", text, avoid=("fs grow",))
    check(results, "B df total 3670016", text, want=("3670016",))
    wink = msh_wink(text)
    check(results, "C ps lists msh", "msh wink %s" % wink if wink else "",
          want=("msh wink",))
    # Same two things as the live path: the fault wording differs per arch, and
    # RV32 cannot deny rodata or address zero, so only three of the five fault.
    fault_mark = "access fault" if run_checks_replay["rv"] else "MemManage"
    faults = text.count(fault_mark)
    want_faults = 3 if run_checks_replay["rv"] else 5
    check_ok(results, "D prot cases fault", faults >= want_faults,
             "%d/%d" % (faults, want_faults))
    check(results, "E deep 3 grows the stack", text, want=("stack grown",))
    check(results, "E deep 3 journal intact", text, want=("journal intact",))
    if IMAGES[image]["cpus"] > 1:
        m = re.search(r"speedup:\s*(\d+)\.(\d+)x", text)
        ratio = (int(m.group(1)) * 100 + int(m.group(2))) if m else 0
        check_ok(results, "F smpbench >= 1.8x", ratio >= 180,
                 ("%.2fx" % (ratio / 100.0)) if m else "no ratio in the log")


def report(results, image):
    print("\n%s" % ("image " + image if image else ""))
    for name, ok, _text, missing, bad, note in results:
        line = "  %s  %s" % ("PASS" if ok else "FAIL", name)
        if note:
            line += "  (%s)" % note
        print(line)
        if not ok:
            if missing:
                print("        missing: %s" % ", ".join(missing))
            if bad:
                print("        forbidden: %s" % ", ".join(bad))
    passed = sum(1 for r in results if r[1])
    print("\n%d/%d checks passed" % (passed, len(results)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True, choices=sorted(IMAGES))
    ap.add_argument("-p", "--port", default="COM14",
                    help="UART0 by default; the native CDC port needs --dtr")
    ap.add_argument("--dtr", action="store_true", help="assert DTR (CDC console)")
    ap.add_argument("--uf2", help="flash this image first, without touching RTS")
    ap.add_argument("--no-flash", action="store_true",
                    help="talk to a board that is already running")
    ap.add_argument("--first-boot", type=float, default=25.0,
                    help="seconds to wait for the prompt after a flash")
    ap.add_argument("--first-byte-max", type=float, default=5.0,
                    help="a flash-then-boot that is silent longer than this"
                         " fails (the 33.4 bug was 120 s of silence)")
    ap.add_argument("--dd-rounds", type=int, default=0)
    ap.add_argument("--replay", help="check a saved console log instead")
    ap.add_argument("--log", help="where to save the raw console log")
    args = ap.parse_args()

    results = []

    if args.replay:
        with open(args.replay, encoding="utf-8", errors="replace") as f:
            text = f.read()
        replay_checks(text, args.image, results)
        report(results, args.image + " (replay)")
        return 0 if all(r[1] for r in results) else 1

    try:
        con = Console(args.port, dtr=args.dtr)
    except serial.SerialException as e:
        print("cannot open %s: %s" % (args.port, e))
        print("  (UART0 stays present while the board is in BOOTSEL; the native"
              " CDC port does not)")
        return 2

    try:
        if args.uf2 and not args.no_flash:
            print("flashing %s with the console already open ..." % args.uf2)
            root = fw.wait_drive(30, tick=lambda: con.pump(0.05))
            if not root:
                print("no BOOTSEL drive: hold BOOTSEL and replug, or --no-flash")
                return 2
            # Before the flash: flash() pumps the port while it waits for the
            # drive to unmount, and the shell can be up by then (measured
            # 0.35 s), so the [A] line lands in the log *during* the flash.
            # Taking the offset afterwards would cut exactly that line out.
            mark = len(con.log)
            fw.flash(args.uf2, root, tick=lambda: con.pump(0.05))
            boot_text, first, took = wait_boot(con, args.first_boot, mark)
            if first is None:
                root, _ = fw.bootsel_drive()
                print("first boot after the flash: NOTHING on the wire in %.0fs"
                      % args.first_boot)
                print("  BOOTSEL drive still mounted -> the chip never left the"
                      " ROM: flashing/reboot path, not the clock tree"
                      if root else
                      "  no BOOTSEL drive -> our image runs but is silent this"
                      " early: see OPEN-ITEMS 33.4")
                check_ok(results, "0 flash-then-boot without RESET", False,
                         "no output in %.0fs" % args.first_boot)
            else:
                print("first byte %.2fs after the flash, prompt %.2fs"
                      " (no RESET was pressed)" % (first, took))
                check_ok(results, "0 flash-then-boot without RESET (%gs)" % first,
                         first <= args.first_byte_max,
                         "first byte %.2fs, limit %.0fs"
                         % (first, args.first_byte_max))
        else:
            boot_text = con.prompt(args.first_boot)

        if "TinyOS" not in boot_text and "[A]" not in boot_text:
            boot_text += con.prompt(10.0)
        check(results, "0 boot banner", boot_text,
              want=("TinyOS - a tiny multi-platform operating system",)
              if "TinyOS" in boot_text else ("[A]",))
        run_checks(con, args.image, results, args, boot_text)

        log = args.log or os.path.join(
            "build", "sizeprobe",
            "accept33-%s-%s.log" % (args.image, time.strftime("%m%d-%H%M%S")))
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
