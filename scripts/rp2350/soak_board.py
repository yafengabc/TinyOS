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
"""
import io
import re
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM14"
CYCLES = int(sys.argv[2]) if len(sys.argv) > 2 else 8
LOG = sys.argv[3] if len(sys.argv) > 3 else "build/soak-board.log"

CYCLE = ["deep 3", "mem", "wheap", "piperace", "ls /bin | wc",
         "prot rodata", "prot heap", "prot stack", "prot null", "ps"]

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
                t2 = time.time() + 0.25
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


free_before = run("free")
say("free before:")
for ln in free_before.strip().splitlines():
    say("   " + ln.strip())

deep_ok = race_ok = 0
died = None
for c in range(CYCLES):
    for cmd in CYCLE:
        try:
            t = run(cmd)
        except serial.SerialException as e:
            died = "cycle %d, command %r: %r" % (c + 1, cmd, e)
            break
        low = t.lower()
        if cmd == "deep 3":
            if "stack grown" in low and "journal intact" in low:
                deep_ok += 1
        elif cmd == "piperace":
            if "piperace: ok" in low:
                race_ok += 1
        if "kernel panic" in low or "system halted" in low:
            died = "cycle %d, command %r: panic in output" % (c + 1, cmd)
            break
    if died:
        break
    say("cycle %d/%d done  deep=%d piperace=%d" % (c + 1, CYCLES, deep_ok, race_ok))

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
if deep_ok < CYCLES:
    problems.append("deep 3 grew the stack %d/%d times" % (deep_ok, CYCLES))
if race_ok < CYCLES:
    problems.append("piperace ok %d/%d" % (race_ok, CYCLES))

say("")
say("%-8s %s  (%d cycles)" % (PORT, "PASS" if not problems else "FAIL", CYCLES))
for p in problems:
    say("    " + p)
for ln in (ps + uptime).strip().splitlines()[-6:]:
    say("    | " + ln.strip())

out.close()
s.close()
sys.exit(0 if not problems else 1)
