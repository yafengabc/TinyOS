#!/usr/bin/env python3
"""Extract what the probes printed out of a captured UART stream.

The probes emit bare characters with no framing, so a raw capture is mostly
ROM boot text with the payload buried somewhere in it.  Everything the ROM
prints is printable ASCII too, so the payload cannot be found by filtering on
the bytes; it is found positionally instead, as whatever follows an "entry
0x..." line and precedes the next ROM banner.

Every boot contributes one such window, and only the first few bytes of each
are meaningful, so all windows are reported rather than just the last one.
Reporting only the last one is what made an earlier round of these probes look
like a hardware failure: the final window in a capture is usually empty
because the capture ends shortly after a reset.
"""

import re
import sys

data = open(sys.argv[1], "rb").read()

for m in re.finditer(rb"load:0x([0-9a-f]+),len:0x([0-9a-f]+)", data):
    print(f"  segment 0x{m.group(1).decode()} len 0x{m.group(2).decode()}")

if b"SHA-256 comparison failed" in data:
    print("  ROM reported a bad digest (it boots anyway)")

windows = []
for m in re.finditer(rb"entry 0x[0-9a-f]+\r\n", data):
    rest = data[m.end():].split(b"ESP-ROM:")[0]
    rest = bytes(c for c in rest if c >= 0x20 or c in (9, 10, 13))
    if rest:
        windows.append(rest)

if not windows:
    print("  payload: nothing (the program reset before printing, or never ran)")
else:
    for w in windows:
        shown = "".join(chr(c) if 0x20 <= c < 0x7F else f"\\x{c:02x}" for c in w)
        print(f"  payload: {shown!r}  hex {w.hex(' ')}")
