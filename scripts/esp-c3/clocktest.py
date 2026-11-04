#!/usr/bin/env python3
"""Measure the real C3 kernel clock against the host wall clock.

Robust variant of the earlier 2-point probe:
  * reset the board via the EN line (RTS pulse), capture the WHOLE boot
    banner including the "[clk] ..." line that tells us whether the PLL
    actually came up;
  * once the shell prompt appears, send "uptime" every SAMPLE seconds and
    record (host monotonic timestamp, kernel_ms);
  * fit a straight line kernel_ms = a + b * host_seconds; b is the ratio
    (1000 = perfect, <1000 = kernel slow, >1000 = kernel fast).

A single b near 1000 means the tick really runs at 100 Hz regardless of
what the PLL did.  A b near 250 means the PLL readback lied: apb_hz was
set to 80 MHz but the silicon stayed at 20, so the timer divider is 4x
too big and the clock runs at 25 Hz.
"""
import re
import sys
import time
import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
BAUD = 115200
SAMPLE = 2.0          # seconds between uptime probes
WINDOW = 30.0         # total measurement window
WAIT_BOOT = 25.0      # seconds to wait for the prompt

UPTIME_RE = re.compile(r"uptime:\s*(\d+)\.(\d{3})\s*s")

def reset_and_boot(port):
    port.dtr = False   # IO9 high -> boot from flash
    port.rts = True    # EN low  -> hold in reset
    time.sleep(0.15)
    port.reset_input_buffer()
    port.rts = False   # release -> boot
    time.sleep(0.2)

def main():
    ser = serial.Serial(PORT, BAUD, timeout=0.05)
    time.sleep(0.2)
    reset_and_boot(ser)

    boot = bytearray()
    clk_line = None
    deadline = time.time() + WAIT_BOOT
    while time.time() < deadline:
        chunk = ser.read(1024)
        if not chunk:
            continue
        boot += chunk
        sys.stdout.write(chunk.decode("utf-8", "backslashreplace"))
        sys.stdout.flush()
        if b"[clk]" in boot and clk_line is None:
            # grab the [clk] line(s)
            for line in boot.decode("utf-8", "backslashreplace").splitlines():
                if "[clk]" in line:
                    clk_line = line
        if b"tos> " in boot:
            break

    print("\n\n==== [clk] line captured: %r" % (clk_line,))

    # drain any pending and make sure we are at a prompt
    ser.write(b"\r\n")
    time.sleep(0.3)
    ser.read(4096)

    samples = []   # (host_s, kernel_ms)
    t0 = time.perf_counter()
    n = int(WINDOW / SAMPLE)
    for i in range(n + 1):
        ser.write(b"uptime\r\n")
        # read until we get the uptime line or timeout
        got = None
        td = time.time() + 1.5
        while time.time() < td:
            chunk = ser.read(1024)
            if not chunk:
                continue
            text = chunk.decode("utf-8", "backslashreplace")
            m = UPTIME_RE.search(text)
            if m:
                intp = int(m.group(1)); frac = int(m.group(2))
                got = intp * 1000 + frac
                break
        if got is not None:
            host_s = time.perf_counter() - t0
            samples.append((host_s, got))
            print("sample %2d  host=%7.3fs  kernel=%8d ms" % (i, host_s, got))
        else:
            print("sample %2d  (no uptime parsed)" % i)
        # align to schedule
        while time.perf_counter() - t0 < (i + 1) * SAMPLE:
            time.sleep(0.01)

    ser.close()

    if len(samples) < 3:
        print("[fail] too few samples")
        return 1

    # least-squares fit kernel_ms = a + b*host_s
    N = len(samples)
    sx = sum(s for s, _ in samples)
    sy = sum(k for _, k in samples)
    sxx = sum(s * s for s, _ in samples)
    sxy = sum(s * k for s, k in samples)
    denom = N * sxx - sx * sx
    if denom == 0:
        print("[fail] degenerate")
        return 1
    b = (N * sxy - sx * sy) / denom
    a = (sy - b * sx) / N
    print("\n==== fit: kernel_ms = %.3f + %.3f * host_s" % (a, b))
    print("==== ratio b = %.4f  (1000=perfect, 250 => 25 Hz PLL-lie)" % b)
    return 0

if __name__ == "__main__":
    sys.exit(main())
