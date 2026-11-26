#!/usr/bin/env python3
"""One-command acceptance for the RP2040 port (rpi-pico / rpi-pico-smp).

This is the board row of docs/TESTING.md section 5.3: identity, clock, heap,
filesystem, exec, write-back, fault isolation, the long-output check issue #46
was about, two write/verify/unlink rounds, and the on-board toolchain
(csc -> rvm -> rcc -> native for /tests/cover.c, /tests/state.c and
/tests/ready.c, each compared against tests/onboard/*.expected).  Every item is
a prompt-send-expect cycle, so a failure names the command instead of "the
board did something odd".

The toolchain chain is here on purpose: accept33.py runs it as "G" and the C3
acceptance runs it per script, so leaving it out of the RP2040 row meant the
board path was only covered when somebody remembered to type the four commands.

    python scripts/rp2040/accept.py --uf2 build/tinyos-rpi-pico.uf2
    python scripts/rp2040/accept.py --uf2 build/tinyos-rpi-pico-smp.uf2 -p COM19
    python scripts/rp2040/accept.py -p COM19                 # already running

Flashing needs one human action: on this board BOOTSEL is only reachable with
the physical button (scripts/rp2040/autoflash.py has the measurements).  The
script says when to press it, waits for the RPI-RP2 volume, copies, and then
attaches the console - so the manual step is one button press, not a round
trip through the assistant.

The console matters for the long-output check.  When the board's own USB CDC
(2e8a:000a) is attached it is preferred and the check covers the USB path, the
one that truncated at ~192 B.  Over the CH343 UART0 the same check covers only
tty/UART pacing, and the summary says so.

Exit: 0 all checks passed, 1 a check failed, 2 setup (no port, flash failed).
"""

import argparse
import os
import re
import shutil
import string
import sys
import time

import serial
import serial.tools.list_ports

PROMPT = "tos> "
BAD = ("KERNEL PANIC", "System halted", "HardFault")

RP_CDC_VID = 0x2E8A
RP_CDC_PID = 0x000A          # TinyOS's own CDC-ACM
CH343_VID = 0x1A86
CH343_PID = 0x55D3           # the USB-TTL adapter wired to UART0


# --------------------------------------------------------------------------
# port / flash plumbing

def pick_port(want):
    """(port, is_cdc) for the board on the bench; (None, False) if absent."""
    ports = list(serial.tools.list_ports.comports())
    if want and want != "auto":
        cdc = any(p.device == want and p.vid == RP_CDC_VID and
                  p.pid == RP_CDC_PID for p in ports)
        return want, cdc
    for p in ports:
        if p.vid == RP_CDC_VID and p.pid == RP_CDC_PID:
            return p.device, True
    uarts = [p.device for p in ports if p.vid == CH343_VID and p.pid == CH343_PID]
    if len(uarts) == 1:
        return uarts[0], False
    return None, False


def bootsel_drive():
    for letter in string.ascii_uppercase:
        root = letter + ":/"
        if os.path.isfile(root + "INFO_UF2.TXT"):
            return root
    return None


def flash(uf2, bootsel_timeout):
    """Copy `uf2` to the BOOTSEL volume.  True when the copy was issued."""
    if not os.path.isfile(uf2):
        print("missing image: %s" % uf2, file=sys.stderr)
        return False

    print("flash: image %s (%d bytes)" % (uf2, os.path.getsize(uf2)))
    drv = bootsel_drive()
    if not drv:
        print("flash: put the board in BOOTSEL - hold the button while "
              "plugging the USB cable in.  Waiting up to %.0fs ..."
              % bootsel_timeout, flush=True)
        t0 = time.time()
        last = t0
        while time.time() - t0 < bootsel_timeout:
            drv = bootsel_drive()
            if drv:
                break
            if time.time() - last >= 10.0:
                last = time.time()
                print("  ... still waiting for the RPI-RP2 volume (%.0fs)"
                      % (time.time() - t0), flush=True)
            time.sleep(0.2)
        if not drv:
            print("flash: no BOOTSEL volume after %.0fs" % bootsel_timeout,
                  file=sys.stderr)
            return False
    print("flash: BOOTSEL volume %s" % drv)

    t0 = time.time()
    try:
        shutil.copyfile(uf2, drv + os.path.basename(uf2))
    except OSError as exc:
        print("flash: copy failed: %s" % exc, file=sys.stderr)
        return False
    print("flash: copied in %.2fs" % (time.time() - t0))

    # The volume vanishes when the write lands; the board is booting by then.
    for _ in range(300):
        if not bootsel_drive():
            print("flash: drive ejected, board rebooted")
            return True
        time.sleep(0.15)
    print("flash: WARNING volume still mounted after 45s", file=sys.stderr)
    return True


# --------------------------------------------------------------------------
# console

class Board:
    def __init__(self, port, dtr=False, baud=115200):
        self.s = serial.Serial(port, baud, timeout=0.05)
        self.s.dtr = bool(dtr)
        self.s.rts = False
        self.s.reset_input_buffer()

    def close(self):
        try:
            self.s.close()
        except serial.SerialException:
            pass

    def read_until_prompt(self, timeout=15.0):
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.s.read(512)
            if chunk:
                buf += chunk
                if buf.decode("utf-8", "replace").endswith(PROMPT):
                    break
        return buf.decode("utf-8", "replace")

    def cmd(self, line, timeout=15.0):
        self.s.write(line.encode() + b"\r\n")
        return self.read_until_prompt(timeout)


def attach(port, dtr, tries=60):
    """Open the console, surviving the USB re-enumeration after a flash."""
    err = None
    for _ in range(tries):
        try:
            return Board(port, dtr=dtr)
        except serial.SerialException as exc:
            err = exc
            time.sleep(0.5)
    print("cannot open %s: %s" % (port, err), file=sys.stderr)
    return None


# --------------------------------------------------------------------------
# transcript helpers

def strip_echo(text, echo):
    """Drop the echoed command line and any CRLF the console adopted."""
    nl = text.find("\n")
    if nl >= 0 and echo.strip() and echo.strip() in text[:nl]:
        text = text[nl + 1:]
    return text.lstrip("\r\n")


def tokens(text, echo):
    """Whitespace-separated words in a command's output, prompt removed."""
    body = strip_echo(text, echo).replace(PROMPT, " ")
    return [t for t in re.split(r"\s+", body) if t]


def output_lines(text, echo):
    """A command's own output: echo, prompt and rvm's timing trailer gone."""
    lines = [l.strip() for l in text.replace("\r", "").splitlines()]
    if lines and lines[0] == echo.strip():
        lines = lines[1:]
    return [l for l in lines
            if l and not l.startswith("tos>") and not l.startswith("result=")]


ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def expected_lines(name):
    path = os.path.join(ROOT, "tests", "onboard", name)
    with open(path, encoding="utf-8") as f:
        return [l.strip() for l in f if l.strip()]


def body_exact(text, echo):
    """A command's raw output, so the caller can compare byte counts.

    Keeps CRLF: the console turns every \\n into \\r\\n, which is part of the
    count the check compares against wc's byte total.
    """
    body = strip_echo(text, echo)
    if body.rstrip().endswith(PROMPT.strip()):
        # the prompt follows the last byte; rstrip() ate the prompt's space
        idx = body.rfind(PROMPT.strip())
        if idx >= 0 and not body[idx + len(PROMPT.strip()):].strip():
            body = body[:idx]
    return body


# --------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="auto",
                    help="serial port, or 'auto' for the board's CDC/UART")
    ap.add_argument("--dtr", action="store_true",
                    help="assert DTR (implied for the USB CDC console)")
    ap.add_argument("--target", default=None,
                    help="recorded in the report; default is taken from --uf2")
    ap.add_argument("--uf2", default=None,
                    help="flash this image first (one BOOTSEL button press)")
    ap.add_argument("--bootsel-timeout", type=float, default=180.0)
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args()

    target = args.target
    if target is None and args.uf2:
        target = os.path.basename(args.uf2).replace("tinyos-", "").replace(".uf2", "")

    # Flash first: while the board sits in BOOTSEL its native CDC is not
    # enumerated, so picking the port before the copy makes --uf2 unusable
    # on the board's own USB console (2e8a:000a).  UART0 stays present either
    # way and is unaffected.
    if args.uf2 and not flash(args.uf2, args.bootsel_timeout):
        return 2

    # After a UF2 copy the drive ejects before the native CDC re-enumerates;
    # on this host COM29 showed up a moment later.  Poll briefly instead of
    # declaring the console missing (CH343 UART0, when present, is immediate).
    port, is_cdc = pick_port(args.port)
    for _ in range(75):
        if port is not None:
            break
        time.sleep(0.2)
        port, is_cdc = pick_port(args.port)
    if port is None:
        print("no RP2040 console found: attach the board's CDC (2e8a:000a) "
              "or the CH343 UART, or pass -p", file=sys.stderr)
        return 2
    if is_cdc:
        args.dtr = True

    b = attach(port, args.dtr)
    if b is None:
        return 2

    results = []
    transcript = []

    def check(name, ok, detail=""):
        results.append((name, bool(ok), detail))
        print("%s  %-40s %s" % ("PASS" if ok else "FAIL", name, detail))
        return bool(ok)

    try:
        # A freshly flashed board is printing its banner; a board that was
        # already up answers a bare newline.  One Ctrl-C first, because a
        # program left running by an earlier session would eat the command.
        first = b.read_until_prompt(4.0)
        if PROMPT not in first:
            first += b.cmd("")
        if PROMPT not in first:
            b.s.write(b"\x03\r\n")
            first += b.read_until_prompt(10.0)
        transcript.append(first)
        check("boot: shell prompt", PROMPT in first, "prompt")

        # 1. identity first, and fatal: running the RP2040 acceptance against
        #    another board (the single CH343 on the bench can be a C3) would
        #    otherwise poke at that board's filesystem and report its results.
        cpu = b.cmd("cat /proc/cpuinfo")
        ident = "RP2040" in cpu and "Raspberry Pi Pico" in cpu
        model = re.search(r"model name\s*:\s*(.+)", cpu)
        isa = re.search(r"\bisa\s*:\s*(.+)", cpu)
        check("identity: RP2040 + board name", ident,
              (model.group(1).strip() if model else "?") + " / "
              + (isa.group(1).strip() if isa else "?"))
        if not ident:
            print("\nthis is not an RP2040 image (model=%s isa=%s); stopping "
                  "before it touches that board's filesystem."
                  % (model.group(1).strip() if model else "?",
                     isa.group(1).strip() if isa else "?"), file=sys.stderr)
            return 2

        # 2. the clock: two reads 1 s apart must follow wall time.
        u1t = b.cmd("uptime")
        t1 = time.monotonic()
        time.sleep(1.0)
        u2t = b.cmd("uptime")
        t2 = time.monotonic()
        u1 = re.search(r"uptime:\s*([0-9.]+)\s*s", u1t)
        u2 = re.search(r"uptime:\s*([0-9.]+)\s*s", u2t)
        delta = (float(u2.group(1)) - float(u1.group(1))) if (u1 and u2) else None
        wall = t2 - t1
        check("clock advances with wall time",
              delta is not None and abs(delta - wall) <= 0.15,
              "delta=%.3f s wall=%.3f s" % (delta, wall) if delta is not None
              else "no uptime line")

        # 3. heap: the number is not asserted, but it must parse.
        free = b.cmd("free")
        m = re.search(r"kernel heap\s+(\d+)\s+(\d+)\s+(\d+)", free)
        check("heap reports a free size", bool(m),
              "kernel heap free=%s" % m.group(3) if m else "no kernel heap line")

        ls = b.cmd("ls /bin")
        check("filesystem read: ls /bin", "msh" in ls and "sh" in ls, "msh + sh")

        hello = b.cmd("hello")
        check("exec a user program", "hello from ring 3!" in hello, "hello")

        b.cmd("wfile acc.tmp x")
        cat = b.cmd("cat acc.tmp")
        rm = b.cmd("rm acc.tmp")
        check("filesystem write-back", "x" in cat and PROMPT in rm,
              "wfile/cat/rm")

        # 3b. exec() path rules: a relative name is resolved against the cwd
        #     and a bare name looks there first, then in /bin.  Copy a program
        #     into the cwd and run it both ways.
        b.cmd("cat /bin/hello > acc_h")
        dotted = b.cmd("./acc_h")
        bare = b.cmd("acc_h")
        b.cmd("rm acc_h")
        check("exec: ./name and bare name run from the cwd",
              "hello from ring 3!" in dotted and
              "hello from ring 3!" in bare, "cwd exec")

        crash = b.cmd("crash")
        # The fault dump goes to UART0 only on this port: the M0+ runs
        # HardFault above every interrupt, so while the handler prints neither
        # the USB interrupt (IN completion) nor the tick (CDC ring pump) can
        # run, and a CDC console would take the first packet and then need the
        # port reopened.  board/rpi-pico implements plat_console_mirror() to
        # mute the mirror for that report.  The UART0 row still checks this
        # strictly; on a CDC console the item is reported as skipped, not
        # failed, because the console under test is not the fault console.
        if is_cdc:
            check("fault isolation (skipped: dump is UART0-only)",
                  PROMPT in crash and "KERNEL PANIC" not in crash,
                  "cdc console; no dump expected, prompt must return")
        else:
            check("fault isolation",
                  PROMPT in crash and "KERNEL PANIC" not in crash,
                  "crash returned to prompt with its dump")

        # 4. the #46 regression: a >1 KB output must arrive whole.  Compare
        #    the listing with wc over the same directory - the pipe path
        #    cannot truncate, so a dropped console byte shows up here.
        listed = tokens(b.cmd("ls /bin"), "ls /bin")
        wc = b.cmd("ls /bin | wc")
        counts = re.search(r"(\d+)\s+(\d+)\s+(\d+)", wc)
        want_words = int(counts.group(2)) if counts else -1
        check("long output: ls /bin complete",
              len(listed) == want_words > 0,
              "%d names, wc says %d" % (len(listed), want_words))

        # 5. and for a file we can measure exactly: wc gives lines and bytes,
        #    and the console turns each \n into \r\n, so the byte count of the
        #    transcript has to be bytes + lines.
        wc = b.cmd("wc /readme.txt")
        m = re.search(r"(\d+)\s+(\d+)\s+(\d+)\s+/readme.txt", wc)
        if not m:
            check("long output: cat /readme.txt complete", False, "no wc line")
        else:
            lines, words, nbytes = (int(g) for g in m.groups())
            body = body_exact(b.cmd("cat /readme.txt"), "cat /readme.txt")
            transcript.append(body)
            got_lines = body.count("\r\n")
            got_words = len(re.findall(r"\S+", body))
            check("long output: cat /readme.txt complete",
                  len(body) == nbytes + lines and
                  got_lines == lines and got_words == words,
                  "bytes %d/%d lines %d/%d words %d/%d"
                  % (len(body), nbytes + lines, got_lines, lines,
                     got_words, words))

        # 6-7. two write/verify/unlink rounds: the flash block device and the
        #      allocator, not just a read.
        for rnd in (1, 2):
            w = b.cmd("dd of=/acc%d.bin bs=4096 count=1" % rnd)
            v = b.cmd("dd if=/acc%d.bin bs=4096 count=1" % rnd)
            d = b.cmd("rm /acc%d.bin" % rnd)
            ok = "wrote 4096 bytes" in w and "pattern verified" in v and \
                 PROMPT in d
            check("dd/rm round %d" % rnd, ok,
                  "write+verify+unlink" if ok else "see transcript")

        # 8. the on-board toolchain, end to end: csc compiles the scripts that
        #    ship in the image, rvm interprets the RIR, rcc compiles it to a
        #    native image, and both have to print the expected file.  This is
        #    the same chain accept33.py runs as "G" and the C3 acceptance runs
        #    per script - it lives in the acceptance script because nobody
        #    remembers to type it by hand on every board run.
        for src in ("cover", "state", "ready"):
            exp = expected_lines("%s.expected" % src)
            out = b.cmd("csc /tests/%s.c /tests/%s.rir" % (src, src), 90.0)
            check("csc compiles /tests/%s.c" % src,
                  "csc: /tests/%s.c" % src in out, "csc")
            out = b.cmd("rvm /tests/%s.rir" % src, 60.0)
            rvm_lines = output_lines(out, "rvm /tests/%s.rir" % src)
            check("rvm %s matches %s.expected" % (src, src),
                  rvm_lines == exp,
                  "%d/%d lines" % (len(rvm_lines), len(exp)))
            out = b.cmd("rcc /tests/%s.rir /bin/%s" % (src, src), 90.0)
            check("rcc compiles /bin/%s" % src, "rcc:" in out, "rcc")
            out = b.cmd(src, 60.0)
            native_lines = output_lines(out, src)
            check("native %s matches %s.expected" % (src, src),
                  native_lines == exp,
                  "%d/%d lines" % (len(native_lines), len(exp)))
            check("%s: rvm and native agree" % src,
                  rvm_lines == native_lines, "")

        # 9. end where we started: the console is still a console.
        end = b.cmd("uptime")
        transcript.append(end)
        check("console still answers", PROMPT in end, "uptime at the end")
    finally:
        b.close()

    text = "".join(transcript)
    bad = [m for m in BAD if m in text]
    check("no panic/fault in the transcript", not bad, ", ".join(bad))

    passed = sum(1 for _, ok, _ in results if ok)
    total = len(results)
    print()
    print("%s %d/%d" % ("ACCEPT OK" if passed == total else "ACCEPT FAIL",
                        passed, total))
    print("target: %s  port: %s  console: %s"
          % (target or "?", port, "usb-cdc" if is_cdc else "uart"))
    if not is_cdc:
        print("note: long output ran over UART0, not the USB CDC console - "
              "attach the board's CDC (2e8a:000a) for the #46 path")
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
