"""Soak the window allocator and the segment list on the real board.

`scripts/soak.py` runs this load on QEMU only - it drives `tostest smoke`,
which cannot reach a board.  But the failure modes it hunts are cumulative
(a window never handed back, a third segment that goes stale, a stack_low
that stops matching after a growth), and the board is the harsher place to
look: 4 KiB stacks instead of QEMU's 32 KiB, real flash behind the fs, and
two cores that actually run at the same time.

Same load, same assertion: `free` before and after must match except the
heap's peak watermark.

    scripts/rp2350/soak_board.py COM14 8 [log]
    scripts/rp2350/soak_board.py -p COM19 -n 20 --image rpi-pico2-smp
    scripts/rp2350/soak_board.py -p COM19 -m 30 --image rpi-pico2-smp

`-n` counts cycles, `-m` runs for wall-clock minutes.  T4 uses `-m 30`: a cycle
takes a different number of seconds on each board, so "20 cycles" is a
different amount of stress on a Pico 2 than on an RP2040; the same 30 minutes
is comparable across the six images (docs/TESTING.md section 6).

Both boards run the same cycle.  The list is filtered against `ls /bin` first,
so a program this image does not carry is skipped with a note instead of
turning into a failed check - the Pico 2 has no `screen`/`lcdconsole`, and a
run there must still be comparable to the ATK one.  `--no-filter` restores the
old behaviour (send everything, let the shell answer).
"""
import argparse
import io
import re
import sys
import time

import serial

ap = argparse.ArgumentParser()
ap.add_argument("port", nargs="?", default=None)
ap.add_argument("cycles", nargs="?", type=int, default=None)
ap.add_argument("log", nargs="?", default=None)
ap.add_argument("-p", "--port", dest="port_opt", metavar="PORT", default=None)
ap.add_argument("-n", "--cycles", dest="cycles_opt", type=int, metavar="N",
                default=None)
ap.add_argument("-l", "--log", dest="log_opt", metavar="FILE", default=None)
ap.add_argument("--image", default="", help="label for the log, e.g. rpi-pico2-smp")
ap.add_argument("-m", "--minutes", type=float, default=0.0, metavar="MIN",
                help="run for MIN minutes instead of a cycle count (0 = use -n)")
ap.add_argument("--no-filter", action="store_true",
                help="send every command, even ones this image may not carry")
args = ap.parse_args()

PORT = args.port_opt or args.port or "COM14"
CYCLES = args.cycles_opt or args.cycles or 8
MINUTES = args.minutes
LOG = args.log_opt or args.log or "build/soak-board.log"
IMAGE = args.image or PORT

CYCLE = ["deep 3", "mem", "wheap", "piperace", "ls /bin | wc",
         "prot rodata", "prot heap", "prot stack", "prot null", "ps"]

# Commands the shell answers itself; everything else in CYCLE is a program in
# /bin and has to exist in the image under test.
BUILTINS = {"ls", "ps", "free", "mem", "uptime", "help", "cat", "echo",
            "df", "rm", "cd", "pwd", "run", "wheap"}

FREE = re.compile(
    r"kernel heap\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+).*?"
    r"SRAM\s+(\d+)\s+(\d+)\s+kernel\s+(\d+)\s+(\d+)\s+"
    r"tasks\s+(\d+)\s+(\d+)\s+unused\s+(\d+)\s+(\d+)", re.S)

out = io.open(LOG, "w", encoding="utf-8", errors="replace")
T0 = time.time()


def say(msg):
    line = "[%6.1fs] %s" % (time.time() - T0, msg)
    print(line, flush=True)
    out.write(line + "\n")
    out.flush()


s = serial.Serial(PORT, 115200, timeout=0.05)
s.setDTR(False)
s.setRTS(False)
say("port %s open" % PORT)


def read_until(stop, sec):
    buf = bytearray()
    end = time.time() + sec
    while time.time() < end:
        d = s.read(4096)
        if d:
            buf.extend(d)
            if stop in bytes(buf):
                # A program can emit its last byte after the shell has already
                # printed its prompt (seen on RP2040: "piperace: otos> k").
                # Keep draining briefly so the verdict line is complete before
                # run() hands it to the checker.
                t2 = time.time() + 0.75
                while time.time() < t2:
                    m = s.read(4096)
                    if m:
                        buf.extend(m)
                    else:
                        time.sleep(0.01)
                break
        else:
            time.sleep(0.01)
    return bytes(buf).decode("utf-8", "replace")


s.reset_input_buffer()
s.write(b"\r\n")
boot = read_until(b"tos> ", 20)
if "tos> " not in boot:
    say("no prompt on a bare CR - trying once more")
    s.reset_input_buffer()
    s.write(b"\r\n")
    boot = read_until(b"tos> ", 20)
if "tos> " not in boot:
    say("ABORT: no shell prompt")
    out.close()
    s.close()
    sys.exit(2)
say("shell is up")


def run(cmd, sec=40):
    s.reset_input_buffer()
    s.write(cmd.encode() + b"\r\n")
    s.flush()
    t = read_until(b"tos> ", sec)
    out.write("\n### CMD %s\n%s" % (cmd, t))
    out.flush()
    return t


def image_programs():
    """Programs in /bin of the image under test, so one cycle serves both."""
    names = set()
    for line in run("ls /bin").replace("\r", "").splitlines():
        for tok in line.split():
            if re.fullmatch(r"[A-Za-z0-9_.-]+", tok):
                names.add(tok)
    return names


cycle = list(CYCLE)
if not args.no_filter:
    have = image_programs()
    if not have:
        say("could not list /bin: running the unfiltered cycle")
    else:
        kept = []
        for cmd in cycle:
            head = cmd.split()[0]
            if head in BUILTINS or head in have:
                kept.append(cmd)
            else:
                say("skip %r: this image has no %r" % (cmd, head))
        cycle = kept
        if not cycle:
            say("ABORT: none of the cycle is in this image")
            out.close()
            s.close()
            sys.exit(2)

deep_present = "deep 3" in cycle
race_present = "piperace" in cycle


free_before = run("free")
say("free before:")
for ln in free_before.strip().splitlines():
    say("   " + ln.strip())

deep_ok = race_ok = 0
died = None
deadline = (T0 + MINUTES * 60.0) if MINUTES > 0 else None
c = 0
while True:
    # A wall-clock run is what makes the six images comparable: a cycle on the
    # Pico 2 is not the same number of seconds as one on the RP2040, so "20
    # cycles" means different amounts of stress on each board.
    if deadline is not None:
        if time.time() >= deadline:
            break
    elif c >= CYCLES:
        break
    c += 1
    for cmd in cycle:
        try:
            t = run(cmd)
        except serial.SerialException as e:
            died = "cycle %d, command %r: %r" % (c, cmd, e)
            break
        low = t.lower()
        if cmd == "deep 3":
            if "stack grown" in low and "journal intact" in low:
                deep_ok += 1
        elif cmd == "piperace":
            if "piperace: ok" in low:
                race_ok += 1
        if "kernel panic" in low or "system halted" in low:
            died = "cycle %d, command %r: panic in output" % (c, cmd)
            break
    if died:
        break
    if deadline is not None:
        left = max(0.0, deadline - time.time())
        say("cycle %d done  deep=%d piperace=%d  %.0fs left"
            % (c, deep_ok, race_ok, left))
    else:
        say("cycle %d/%d done  deep=%d piperace=%d"
            % (c, CYCLES, deep_ok, race_ok))

free_after = run("free") if not died else ""
ps = run("ps") if not died else ""
uptime = run("uptime") if not died else ""

say("free after:")
for ln in free_after.strip().splitlines():
    say("   " + ln.strip())

rows = [m.groups()[:3] + m.groups()[4:]
        for m in FREE.finditer(free_before + free_after)]
problems = []
if died:
    problems.append("board died - " + died)
if len(rows) < 2:
    problems.append("could not parse both free reports")
elif rows[0] != rows[-1]:
    problems.append("free drifted %s -> %s" % (rows[0], rows[-1]))
ran = c if deadline is not None else CYCLES
if deep_present and deep_ok < ran:
    problems.append("deep 3 grew the stack %d/%d times" % (deep_ok, ran))
if race_present and race_ok < ran:
    problems.append("piperace ok %d/%d" % (race_ok, ran))

say("")
if deadline is not None:
    say("%-24s %s  (%.0f min, %d cycles, %d commands)" %
        (IMAGE, "PASS" if not problems else "FAIL", MINUTES, c, len(cycle)))
else:
    say("%-24s %s  (%d cycles, %d commands)" %
        (IMAGE, "PASS" if not problems else "FAIL", CYCLES, len(cycle)))
for p in problems:
    say("    " + p)
for ln in (ps + uptime).strip().splitlines()[-6:]:
    say("    | " + ln.strip())

out.close()
s.close()
sys.exit(0 if not problems else 1)
