#!/usr/bin/env python3
"""Flash a C3 image and capture whatever it prints on UART0.

The probes in this directory print exactly one character and then spin, so
the usual "flash, exit, open the port" dance loses the byte: esptool's hard
reset fires before the port is reopened.  Here the image is written with
--after no-reset, then we pulse EN ourselves while already listening.

Wiring: RTS -> EN, DTR -> GPIO9 through the board's auto-download circuit.
DTR low means IO9 high, i.e. normal boot rather than download mode.
"""

import subprocess
import sys
import time
from pathlib import Path

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
IMAGE = sys.argv[2] if len(sys.argv) > 2 else "build/exp-c3/iram.bin"
LISTEN = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0

print(f"writing {IMAGE} -> {PORT} @ 0x0", flush=True)
subprocess.run(
    [sys.executable, "-m", "esptool", "--chip", "esp32c3", "-p", PORT,
     "-b", "115200", "--after", "no-reset",
     "write-flash", "0x0", IMAGE],
    check=True,
)

port = serial.Serial(PORT, 115200, timeout=0.05)
port.dtr = False          # IO9 high -> boot from flash, not download
port.rts = True           # EN low -> hold in reset
time.sleep(0.1)
port.reset_input_buffer()
port.rts = False          # EN high -> release
print("--- uart ---", flush=True)

deadline = time.time() + LISTEN
seen = bytearray()
while time.time() < deadline:
    chunk = port.read(64)
    if chunk:
        seen += chunk
        sys.stdout.write(chunk.decode("utf-8", "backslashreplace"))
        sys.stdout.flush()
port.close()
print(f"\n--- {len(seen)} byte(s) ---", flush=True)

# Keep the raw capture so capture.py can pick the payload out of the ROM text.
# The probes emit single bytes with no line framing, which does not survive
# being eyeballed in a terminal.
out = Path(__file__).resolve().parents[2] / "build/exp-c3/last.raw"
out.parent.mkdir(parents=True, exist_ok=True)
out.write_bytes(bytes(seen))
print(f"saved {out}", flush=True)
