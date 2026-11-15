#!/usr/bin/env python3
"""Gate-H hardware acceptance: flash a UF2 onto the RP2350 and run the
kernel-thread test suite over the UART0 console in a single session.

Keeps the COM port open across the whole flash/reboot window (the boot banner
is a one-shot stream), then drives the shell one command at a time and writes
the complete transcript.  Usage:

    python -u scripts/rp2350/kthread_accept.py \
        --uf2 build/rp2350smp.uf2 --port COM14 \
        --out build/test-logs/gate-h-rp2350smp.txt
"""

import argparse
import os
import sys
import time

import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flash as fl  # bootsel_drive(), flash()

MARK = b"tos> "

# (shell line, seconds to wait for the next prompt)
COMMANDS = (
    ("cat /proc/cpuinfo", 12),
    ("cat /proc/dev", 12),
    ("uptime", 12),
    ("free", 12),
    ("smp", 25),
    ("kthreadtest", 150),
    ("threadbench", 300),
    ("prodcons", 90),
    ("smpbench", 240),
    ("ps", 12),
    ("kthreadtest leave", 90),
    ("ps", 12),
)


def drain(s, buf):
    """Read everything currently queued; return the newly read bytes."""
    got = bytearray()
    while True:
        d = s.read(4096)
        if not d:
            break
        got.extend(d)
    buf.extend(got)
    return bytes(got)


def cmd(s, line, timeout):
    """Send one shell line, wait for the next prompt, return (text, prompted)."""
    s.reset_input_buffer()
    s.write(line.encode() + b"\r\n")
    s.flush()
    out = bytearray()
    end = time.time() + timeout
    prompted = False
    while time.time() < end:
        d = s.read(4096)
        if d:
            out.extend(d)
            if MARK in bytes(out):
                # Let any trailing characters land before declaring success.
                tail_end = time.time() + 0.25
                while time.time() < tail_end:
                    more = s.read(4096)
                    if more:
                        out.extend(more)
                        tail_end = time.time() + 0.10
                    else:
                        time.sleep(0.01)
                prompted = True
                break
        else:
            time.sleep(0.01)
    return out.decode("utf-8", "replace"), prompted


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uf2", required=True)
    ap.add_argument("--port", default="COM14")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", required=True)
    ap.add_argument("--no-flash", action="store_true",
                    help="skip the UF2 copy (board already running)")
    ap.add_argument("--soft-bootsel", action="store_true",
                    help="if no BOOTSEL drive is present, send Ctrl-D on the "
                         "open UART console to make the ROM mount it")
    ap.add_argument("--commands", default=None,
                    help="comma-separated shell lines, optional @seconds "
                         "timeout; defaults to the full gate-H suite")
    ap.add_argument("--boot-timeout", type=float, default=45.0)
    args = ap.parse_args()

    if args.commands:
        suite = []
        for item in args.commands.split(","):
            item = item.strip()
            if not item:
                continue
            if "@" in item:
                line, secs = item.rsplit("@", 1)
                suite.append((line, float(secs)))
            else:
                suite.append((item, 60.0))
    else:
        suite = COMMANDS

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    lines = []

    def log(msg):
        print(msg, flush=True)
        lines.append(msg + "\n")

    log("=== gate H hardware acceptance ===")
    log("uf2 : %s (%s bytes)" % (args.uf2, os.path.getsize(args.uf2)))
    log("port: %s @ %d" % (args.port, args.baud))
    log("time: %s" % time.strftime("%Y-%m-%d %H:%M:%S"))

    s = serial.Serial(args.port, args.baud, timeout=0.02)
    s.setDTR(False)
    s.setRTS(False)
    s.reset_input_buffer()
    log("UART0 opened (DTR/RTS de-asserted)")

    boot = bytearray()

    if not args.no_flash:
        root, info = fl.bootsel_drive()
        if not root and args.soft_bootsel:
            log("no BOOTSEL drive: sending Ctrl-D on the open console ...")
            s.write(b"\x04")
            s.flush()
            last_poke = time.time()
            end = time.time() + args.boot_timeout
            while time.time() < end and not root:
                drain(s, boot)
                root, info = fl.bootsel_drive()
                if not root and time.time() - last_poke > 1.5:
                    s.write(b"\x04")
                    s.flush()
                    last_poke = time.time()
                time.sleep(0.2)
        elif not root:
            log("waiting for BOOTSEL drive ...")
            end = time.time() + args.boot_timeout
            while time.time() < end and not root:
                drain(s, boot)
                root, info = fl.bootsel_drive()
                time.sleep(0.3)
        if not root:
            log("ABORT: no BOOTSEL drive")
            return 1
        log("BOOTSEL: %s  %s" % (root, info))
        fl.flash(args.uf2, root, tick=lambda: drain(s, boot))

    log("waiting for shell prompt ...")
    # With --no-flash the board is already idle and will not print another
    # prompt on its own; an empty line makes the shell emit a fresh one.
    if args.no_flash:
        s.write(b"\r\n")
        s.flush()
    end = time.time() + args.boot_timeout
    up = False
    while time.time() < end:
        drain(s, boot)
        if MARK in bytes(boot):
            up = True
            break
        time.sleep(0.02)
    text = boot.decode("utf-8", "replace")
    log("---- boot log (%d bytes) ----" % len(boot))
    lines.append(text + "\n")
    print(text, flush=True)
    log("--------------------------------")
    if not up:
        log("ABORT: no shell prompt (board may be hung)")
        return 1
    log("shell is up")

    results = []
    for line, timeout in suite:
        log("===== %s (timeout %ss) =====" % (line, timeout))
        out, prompted = cmd(s, line, timeout)
        log("prompted=%s bytes=%d" % (prompted, len(out)))
        print(out, flush=True)
        lines.append(out + "\n")
        results.append((line, prompted, len(out)))

    log("===== summary =====")
    for line, prompted, n in results:
        log("  %-24s prompted=%-5s bytes=%d" % (line, prompted, n))

    # A hung shell must not be reported as a pass.
    alive, _ = cmd(s, "echo GATE_H_ALIVE", 12)
    log("final liveness: %s" % ("OK" if "GATE_H_ALIVE" in alive else "NO RESPONSE"))
    lines.append(alive + "\n")

    with open(args.out, "w", encoding="utf-8", errors="replace") as f:
        f.write("".join(lines))
    log("transcript -> %s" % args.out)
    s.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
