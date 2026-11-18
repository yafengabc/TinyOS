#!/usr/bin/env python3
"""Six-item heartbeat for the RP2350 port (ATK, Pico 2 and friends).

This is the board-level minimum from docs/TESTING.md section 5.0: it does not
claim the full acceptance table, only that the image boots, keeps time, reads
and writes the filesystem, can exec a user program, and survives a user fault.

    python scripts/rp2350/heartbeat.py                      # auto port
    python scripts/rp2350/heartbeat.py -p COM19 --target rpi-pico2
    python scripts/rp2350/heartbeat.py -p COM17 --dtr        # USB CDC console

The board must already be running the image; flashing is a separate step
(scripts/rp2350/flash.py).  The six checks are the same as the RP2040 port's
scripts/rp2040/heartbeat.py on purpose: the point of a heartbeat is that every
board answers the same questions the same way.

`--port auto` (the default) picks the board's own USB CDC (2e8a:000a) when it
is present, otherwise the only CH343 UART adapter; with two boards on the
bench, name the port explicitly.

Output is one line per item and a final `HEARTBEAT OK n/n` (exit 0) or
`HEARTBEAT FAIL n/n` (exit 1).  When the shell never appears the report says
so instead of pretending six checks ran: "no shell" and "booted but item 3
failed" are different diagnosis paths.
"""

import argparse
import re
import sys
import time

import serial
import serial.tools.list_ports

RP_CDC_VID = 0x2E8A
RP_CDC_PID = 0x000A          # TinyOS's own CDC-ACM
CH343_VID = 0x1A86
CH343_PID = 0x55D3           # the USB-TTL adapter used for UART0


def find_port():
    """Return (port, is_cdc) for the board on the bench, or (None, False)."""
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if p.vid == RP_CDC_VID and p.pid == RP_CDC_PID:
            return p.device, True
    uarts = [p.device for p in ports if p.vid == CH343_VID and p.pid == CH343_PID]
    if len(uarts) == 1:
        return uarts[0], False
    return None, False


class Console:
    def __init__(self, port, dtr=False):
        self.s = serial.Serial(port, 115200, timeout=0.1)
        self.s.dtr = bool(dtr)
        self.s.reset_input_buffer()

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def prompt(self, timeout=10.0):
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.s.read(4096)
            if chunk:
                buf += chunk
                if buf.decode("utf-8", "replace").rstrip().endswith("tos>"):
                    break
        return buf.decode("utf-8", "replace")

    def cmd(self, line, timeout=10.0):
        self.s.write(line.encode() + b"\r\n")
        return self.prompt(timeout)


def uptime_seconds(text):
    m = re.search(r"uptime:\s*([0-9]+(?:\.[0-9]+)?)\s*s", text)
    return float(m.group(1)) if m else None


def main(default_target="rpi-pico2"):
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="auto",
                    help="serial port, or 'auto' to find the board's CDC/UART")
    ap.add_argument("--target", default=default_target,
                    help="recorded in the report; the script itself is board-agnostic")
    ap.add_argument("--dtr", action="store_true", help="assert DTR (USB CDC console)")
    ap.add_argument("--not-shell-timeout", type=float, default=10.0,
                    help="seconds to wait for the first prompt")
    args = ap.parse_args()

    dtr = args.dtr
    port = args.port
    if port == "auto":
        port, is_cdc = find_port()
        if not port:
            print("cannot pick a port: two or more adapters are attached; pass -p COMx")
            print("\nHEARTBEAT FAIL 0/6  (no port)")
            return 2
        dtr = dtr or is_cdc
        print("port %s (%s)" % (port, "USB CDC" if is_cdc else "UART0 adapter"))

    try:
        con = Console(port, dtr=dtr)
    except serial.SerialException as e:
        print("cannot open %s: %s" % (port, e))
        print("\nHEARTBEAT FAIL 0/6  (port busy)")
        return 2

    results = []

    def check(name, ok, detail=""):
        results.append((name, bool(ok), detail))
        print("%s  %-26s  %s" % ("PASS" if ok else "FAIL", name, detail))
        return bool(ok)

    try:
        boot = con.cmd("", args.not_shell_timeout)
        if "tos>" not in boot:
            # One misdiagnosis this whole port burned time on: "no bytes at
            # all" and "shell is up but a command failed" looked the same.
            print("no shell prompt on %s after %.0f s"
                  % (port, args.not_shell_timeout))
            if not boot.strip():
                print("  nothing came back at all: wrong port, board not running, "
                      "or (Pico 2 clone) a flash that needs one RUN press")
            else:
                print("  got %r" % boot.strip()[:200])
            print("\nHEARTBEAT FAIL 0/6  (no shell)")
            return 1
        check("1 boot to a shell", True, "prompt seen")

        u1t = con.cmd("uptime")
        t1 = time.monotonic()
        u1 = uptime_seconds(u1t)
        time.sleep(1.0)
        u2t = con.cmd("uptime")
        t2 = time.monotonic()
        u2 = uptime_seconds(u2t)
        delta = (u2 - u1) if (u1 is not None and u2 is not None) else None
        wall = t2 - t1
        # The two uptime reads are separated by the host sleep *and* the serial
        # round trip, so compare the board's delta with the measured wall time
        # rather than assuming the host sleep is the whole interval.
        ok = delta is not None and abs(delta - wall) <= 0.1
        check("2 time advances 1 s", ok,
              "delta=%.3f s, wall=%.3f s" % (delta, wall)
              if delta is not None else "no uptime line")

        ls = con.cmd("ls /bin")
        check("3 filesystem read", bool(re.search(r"\bmsh\b", ls)), "ls /bin")

        hello = con.cmd("hello")
        check("4 user program", "hello from ring 3!" in hello, "hello")

        con.cmd("wfile hb.tmp x")
        cat = con.cmd("cat hb.tmp")
        rm = con.cmd("rm hb.tmp")
        check("5 filesystem write-back", "x" in cat and "tos>" in rm,
              "wfile/cat/rm")

        crash = con.cmd("crash")
        check("6 fault isolation", "tos>" in crash and "KERNEL PANIC" not in crash,
              "crash returned to prompt")
    finally:
        con.close()

    passed = sum(1 for _, ok, _ in results if ok)
    total = len(results)
    print("\n%s %d/%d" % ("HEARTBEAT OK" if passed == total else "HEARTBEAT FAIL",
                          passed, total))
    print("target: %s  port: %s" % (args.target, port))
    return 0 if passed == total else 1


if __name__ == "__main__":
    sys.exit(main())
