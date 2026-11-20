#!/usr/bin/env python3
"""Watch a reboot from both consoles and say who is alive while it is silent.

The RV32 SMP port sometimes takes 33.5 s to come back after `reboot`, and the
only thing a single channel can report is "nothing arrived".  That is not a
diagnosis: a board whose UART transmitter is mis-clocked, a board whose shell
never gets a tick, and a board that is not running at all produce the same
silence.  This script watches the three things that tell them apart:

  UART0      the console under test, read continuously
  CDC        the device node Windows has for the board's USB CDC - present
             means the firmware got through tud_init() and raised the pull-up,
             which is only possible if the CPU is executing that code
  CDC answer whether a command sent to that node is answered, i.e. whether the
             shell is actually scheduled

A cycle is "slow" when the UART prompt takes more than --slow seconds to
appear.  Slow cycles print a timeline; fast ones print one line.

Usage:
    python scripts/rp2350/stall_probe.py -n 10
"""

import argparse
import sys
import threading
import time

import serial
import serial.tools.list_ports as list_ports

PICO_VID_PID = "2E8A:000A"


def find_cdc():
    for p in list_ports.comports():
        if PICO_VID_PID in (p.hwid or ""):
            return p.device
    return None


class CdcWatcher(threading.Thread):
    """Poll the CDC node: is it enumerated, and does it answer `uptime`."""

    def __init__(self, stop_evt, t0):
        super().__init__(daemon=True)
        self.stop_evt = stop_evt
        self.t0 = t0
        self.events = []          # (t, kind, detail)
        self.last_dev = None
        self.last_answer = None

    def _note(self, kind, detail=""):
        self.events.append((round(time.time() - self.t0, 2), kind, detail))

    def run(self):
        while not self.stop_evt.is_set():
            dev = find_cdc()
            if dev != self.last_dev:
                self._note("cdc", dev or "gone")
                self.last_dev = dev
            if dev:
                answer = ""
                try:
                    s = serial.Serial(dev, 115200, timeout=0.1)
                    s.dtr = True
                    s.rts = True
                    time.sleep(0.12)
                    s.reset_input_buffer()
                    s.write(b"uptime\r")
                    time.sleep(0.3)
                    answer = s.read(600).decode("utf-8", "replace").strip()
                    s.close()
                except Exception as exc:          # port busy / just vanished
                    answer = "<%s>" % type(exc).__name__
                if answer != self.last_answer:
                    self._note("cdc-answer", answer[:120] or "<nothing>")
                    self.last_answer = answer
            time.sleep(0.6)


def run_cycle(uart, slow_s, window_s):
    uart.reset_input_buffer()
    t0 = time.time()
    uart.write(b"reboot\r\n")

    stop_evt = threading.Event()
    watcher = CdcWatcher(stop_evt, t0)
    watcher.start()

    buf = b""
    got = None
    while time.time() - t0 < window_s:
        chunk = uart.read(4096)
        if chunk:
            buf += chunk
            if buf.rstrip().endswith(b"tos>"):
                got = time.time() - t0
                break
    stop_evt.set()
    watcher.join(timeout=2.0)
    return got, len(buf), watcher.events


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uart", default="COM19")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("-n", "--cycles", type=int, default=8)
    ap.add_argument("--slow", type=float, default=3.0,
                    help="a cycle slower than this prints its timeline")
    ap.add_argument("--window", type=float, default=45.0)
    args = ap.parse_args()

    uart = serial.Serial(args.uart, args.baud, timeout=0.05)
    uart.dtr = False
    uart.rts = False
    time.sleep(0.3)

    slow = 0
    for i in range(args.cycles):
        got, nbytes, events = run_cycle(uart, args.slow, args.window)
        verdict = ("%.2f s" % got) if got else "NEVER"
        if got and got <= args.slow:
            print("cycle %2d: %8s  %5d bytes" % (i + 1, verdict, nbytes))
        else:
            slow += 1
            print("cycle %2d: %8s  %5d bytes   <== SLOW" % (i + 1, verdict, nbytes))
            for t, kind, detail in events:
                print("    %6.2f  %-11s %s" % (t, kind, detail))
        sys.stdout.flush()
        time.sleep(0.4)

    uart.close()
    print("slow: %d/%d" % (slow, args.cycles))


if __name__ == "__main__":
    main()
