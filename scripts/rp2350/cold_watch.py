#!/usr/bin/env python3
"""Watch a series of manual power cycles and judge each cold boot.

The tester unplugs/replugs power by hand; this process keeps UART0 open across
the whole series, counts boot banners, and records for every cycle whether the
shell came up and whether a trap was printed.  UART0 is the primary witness:
a board that fails to enumerate CDC can still be alive and talking on the wire.

Usage:
    python scripts/rp2350/cold_watch.py --log build/cold-nowait.log --seconds 1200

Stop with Ctrl-C (or let --seconds expire) once the requested number of power
cycles has been performed.  Nothing is written to the board.
"""

import argparse
import os
import re
import sys
import time

import serial
import serial.tools.list_ports

BANNER = re.compile(rb"\[A\] pre-pll|\[rp2040\] boot clk=")
TRAP = re.compile(rb"mcause=|ebreak|load/store access fault|InstFetch")
SHELL = re.compile(rb"TinyOS shell")
USB_DONE = re.compile(rb"\[usb\] init done")
PROMPT = re.compile(rb"tos> ")


def cdc_present():
    return any(p.pid == 10 for p in serial.tools.list_ports.comports())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uart", default="COM19")
    ap.add_argument("--log", default="build/cold-nowait.log")
    ap.add_argument("--seconds", type=float, default=1200.0)
    args = ap.parse_args()

    os.makedirs(os.path.dirname(args.log) or ".", exist_ok=True)
    log = open(args.log, "ab", buffering=0)
    s = None
    start = time.time()
    end = start + args.seconds
    boots = 0
    shell_count = 0
    prompt_count = 0
    traps = 0
    in_boot = False
    cdc_prev = None
    pending = b""

    def emit(line):
        stamp = f"[{time.time() - start:8.2f}] ".encode()
        log.write(stamp + line + b"\n")
        print((stamp + line).decode("utf-8", "replace"), flush=True)

    emit(b"watch started; waiting for power cycles")
    try:
        while time.time() < end:
            if s is None:
                try:
                    s = serial.Serial(args.uart, 115200, timeout=0.1)
                    s.setDTR(False)
                    s.setRTS(False)
                    s.reset_input_buffer()
                    emit(b"UART0 opened")
                except serial.SerialException as exc:
                    emit(b"UART0 open failed: " + str(exc).encode())
                    time.sleep(1.0)
                    continue
            try:
                data = s.read(4096)
            except serial.SerialException as exc:
                emit(b"UART0 read failed: " + str(exc).encode())
                s.close()
                s = None
                continue

            if data:
                log.write(data)
                pending += data
                if len(pending) > 1 << 20:
                    pending = pending[-(1 << 16):]
                for line in pending.splitlines(keepends=True):
                    if BANNER.search(line):
                        boots += 1
                        in_boot = True
                        emit(f"BOOT #{boots}: banner".encode())
                    if USB_DONE.search(line):
                        emit(f"BOOT #{boots}: usb init done".encode())
                    if TRAP.search(line):
                        traps += 1
                        emit(b"BOOT #%d: TRAP %s" % (boots, line.strip()))
                    if SHELL.search(line):
                        shell_count += 1
                        emit(f"BOOT #{boots}: shell banner".encode())
                    if PROMPT.search(line):
                        prompt_count += 1
                        emit(f"BOOT #{boots}: prompt".encode())
                pending = b""

            cdc = cdc_present()
            if cdc_prev is None:
                cdc_prev = cdc
                emit(("CDC node: " + ("present" if cdc else "absent")).encode())
            elif cdc != cdc_prev:
                cdc_prev = cdc
                emit(("CDC node -> " + ("present" if cdc else "absent")).encode())
            time.sleep(0.1)
    except KeyboardInterrupt:
        emit(b"watch stopped by operator")
    finally:
        emit(("SUMMARY boots=%d shell=%d prompt=%d traps=%d" %
              (boots, shell_count, prompt_count, traps)).encode())
        log.close()
        if s:
            s.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
