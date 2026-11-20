"""Measure how long a reboot takes, on both console channels at once.

Built after a scan that looked like "one reboot in five hangs for 34 s": 34 s
is exactly two 16.78 s watchdog periods, and the phase marker in
WATCHDOG_SCRATCH(3) said the doomed boots reached usb_cdc_console_init().  The
open question was which side was lying - the board or the adapter - so this
measures the two witnesses independently and reports both arrival times.

    python scripts/rp2350/reboot_latency.py --cdc COM27 --uart COM19 -n 10

A cycle sends `reboot` over CDC (so the UART is purely a witness), then polls
both channels until each shows a prompt.  Times are from the write.  A channel
that never answers inside the window reports NEVER.
"""

import argparse
import time

import serial
from serial.tools import list_ports


def cdc_ports():
    return [p.device for p in list_ports.comports()
            if "2E8A:000A" in (p.hwid or "").upper()]


def open_quiet(port, dtr):
    try:
        s = serial.Serial(port, 115200, timeout=0.05)
        s.dtr = dtr
        return s
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cdc", default=None)
    ap.add_argument("--uart", default=None)
    ap.add_argument("-n", "--cycles", type=int, default=8)
    ap.add_argument("--window", type=float, default=45.0)
    args = ap.parse_args()

    uart = open_quiet(args.uart, False) if args.uart else None
    cdc = open_quiet(args.cdc or (cdc_ports() or [None])[0], True)

    rows = []
    for i in range(1, args.cycles + 1):
        t0 = time.time()
        if cdc:
            try:
                cdc.write(b"reboot\r\n")
            except Exception:
                pass
            cdc.close()
            cdc = None
        else:
            print("no CDC to send the reset on")
            break

        t_uart = t_cdc = None
        ubuf = b""
        while time.time() - t0 < args.window:
            if uart:
                try:
                    chunk = uart.read(4096)
                except Exception:
                    chunk = b""
                if chunk:
                    ubuf += chunk
                    if t_uart is None and b"tos> " in ubuf:
                        t_uart = time.time() - t0
            if cdc is None:
                port = (cdc_ports() or [None])[0]
                if port:
                    cdc = open_quiet(port, True)
            if cdc:
                try:
                    chunk = cdc.read(4096)
                except Exception:
                    chunk = b""
                    cdc.close()
                    cdc = None
                if chunk and b"tos> " in chunk:
                    t_cdc = time.time() - t0
            if t_uart is not None and t_cdc is not None:
                break
            if t_uart is not None and not args.uart:
                break
        uart_time = "%.2f" % t_uart if t_uart is not None else "NEVER"
        cdc_time = "%.2f" % t_cdc if t_cdc is not None else "NEVER"
        print(f"[{i:2d}/{args.cycles}] uart={uart_time:>7} s  cdc={cdc_time:>7} s"
              f"  ({len(ubuf)} uart bytes)")
        rows.append((t_uart, t_cdc))
        time.sleep(0.4)

    slow = [r for r in rows if (r[0] or 0) > 2 or (r[1] or 0) > 2]
    print(f"\n{len(rows) - len(slow)}/{len(rows)} cycles came back within 2 s")
    if uart:
        uart.close()
    if cdc:
        cdc.close()


if __name__ == "__main__":
    main()
