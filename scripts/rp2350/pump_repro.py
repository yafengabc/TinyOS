#!/usr/bin/env python3
"""Flash an experimental RP2350 image and watch both consoles across the boot.

Built for issue #29: a boot-time USB pump change can wedge the part before the
shell exists, and "no output on CDC" alone cannot tell a dead chip from a host
that has not enumerated yet.  This script keeps UART0 open *before* the UF2 is
copied (the banner is a one-shot stream), then records, for a fixed window:

  - every byte UART0 emitted,
  - whether the native CDC node (2e8a:000a) exists,
  - whether the shell answers a command sent to that node.

Stops the watch early only if a shell answers.  It never tries to rescue the
board on its own: a wedge must be observed, not hidden by a Ctrl-D reboot.

Usage:
    python scripts/rp2350/pump_repro.py --uf2 build/tinyos-rpi-pico2-rv32-smp.uf2 \
        --uart-port COM19 --watch 40 --log build/pump-repro.log
"""

import argparse
import os
import sys
import time

import serial
import serial.tools.list_ports

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from flash import UartTailer, flash  # noqa: E402  (same directory, deliberate)


def cdc_ports():
    return [p.device for p in serial.tools.list_ports.comports() if p.pid == 10]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uf2", default="build/tinyos-rpi-pico2-rv32-smp.uf2")
    ap.add_argument("--drive", default="F:\\")
    ap.add_argument("--uart-port", default="COM19")
    ap.add_argument("--watch", type=float, default=40.0)
    ap.add_argument("--log", default="build/pump-repro.log")
    args = ap.parse_args()

    tail = UartTailer(args.uart_port, 115200)
    tail.tick()
    print(f"UART0 tail: {'open' if tail.s else 'not open (' + str(tail.last_err) + ')'}")

    print(f"flashing {args.uf2} -> {args.drive}")
    if not os.path.isfile(os.path.join(args.drive, "INFO_UF2.TXT")):
        print("BOOTSEL drive not present; put the board into BOOTSEL first",
              file=sys.stderr)
        return 2
    flash(args.uf2, args.drive, tail.tick)

    t_start = time.time()
    t_end = t_start + args.watch
    first_cdc = None
    answered = None
    con = None
    next_probe = time.time() + 2.0
    events = []

    while time.time() < t_end:
        tail.tick()
        ports = cdc_ports()
        if ports and first_cdc is None:
            first_cdc = (time.time() - t_start, ports[0])
            events.append(f"CDC node appeared: {ports[0]}")
            print(events[-1])
            try:
                con = serial.Serial(ports[0], 115200, timeout=0.1)
                con.setDTR(True)          # firmware drops output when DTR is low
                con.setRTS(False)
                con.reset_input_buffer()
            except serial.SerialException as exc:
                events.append(f"CDC open failed: {exc}")
        if con and time.time() >= next_probe:
            next_probe = time.time() + 2.0
            try:
                con.write(b"uptime\r\n")
                con.flush()
                time.sleep(0.4)
                reply = con.read(512)
                if b"uptime:" in reply:
                    answered = (time.time(), reply.decode("utf-8", "replace"))
                    events.append("shell answered: " + answered[1].strip())
                    print(events[-1])
                    break
            except serial.SerialException as exc:
                events.append(f"CDC write failed: {exc}")
                con = None
        time.sleep(0.2)

    raw = bytes(tail.buf)
    os.makedirs(os.path.dirname(args.log) or ".", exist_ok=True)
    with open(args.log, "wb") as f:
        f.write(raw)

    print("\n---- UART0 across the flash and boot ----")
    print(raw.decode("utf-8", "replace") or "(nothing)")
    print("-----------------------------------------")
    if first_cdc:
        print(f"CDC node: {first_cdc[1]} at +{first_cdc[0]:.2f} s from watch start")
    else:
        print("CDC node: NEVER appeared")
    print("shell: " + ("ANSWERED" if answered else "never answered"))
    print(f"UART0 bytes: {len(raw)}; raw log: {args.log}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
