#!/usr/bin/env python3
"""Watch UART0 and the TinyOS CDC port across a physical replug.

Prints every UART0 line as it arrives and reports the moment a new USB CDC
port (VID 2E8A / PID 000A) shows up, so a human replug can be scored without
guessing.  Run it, then unplug/replug the board's native USB cable.

Usage:
    python scripts/rp2350/watch_usb.py -p COM14 --seconds 90
"""

import argparse
import time

import serial

from flash import cdc_port, serial_ports


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="COM14")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=90.0)
    args = ap.parse_args()

    before = {v for _, v in serial_ports()}
    print(f"ports before: {sorted(before)}")
    print(f"CDC before: {cdc_port() or '(none)'}")

    s = serial.Serial(args.port, args.baud, timeout=0.05)
    s.setDTR(False)
    s.setRTS(False)
    print(f"listening on {args.port} for {args.seconds:.0f}s ...", flush=True)

    end = time.time() + args.seconds
    buf = b""
    seen_cdc = cdc_port()
    next_probe = 0.0
    while time.time() < end:
        try:
            chunk = s.read(4096)
        except serial.SerialException as e:
            print(f"\n[uart lost: {e}]", flush=True)
            time.sleep(0.5)
            try:
                s = serial.Serial(args.port, args.baud, timeout=0.05)
                s.setDTR(False)
                s.setRTS(False)
                print("[uart reopened]", flush=True)
            except serial.SerialException:
                pass
            continue
        if chunk:
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                print(line.decode("utf-8", "replace").rstrip(), flush=True)
        now = time.time()
        if now >= next_probe:
            next_probe = now + 0.5
            cur = cdc_port()
            if cur != seen_cdc:
                print(f"\n*** CDC PORT CHANGED: {seen_cdc} -> {cur} ***", flush=True)
                seen_cdc = cur
                if cur:
                    print("*** ENUMERATION SUCCEEDED ***", flush=True)
    s.close()
    print("done.")


if __name__ == "__main__":
    main()
