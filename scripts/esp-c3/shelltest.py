#!/usr/bin/env python3
"""Drive the C3 shell: reset, wait for prompt, send commands, print output."""
import sys, time, serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM4"
CMDS = sys.argv[2:] or ["ls", "cat readme.txt", "hello", "uptime", "echo hi shell"]

p = serial.Serial(PORT, 115200, timeout=0.05)
p.dtr = False
p.rts = True
time.sleep(0.1)
p.reset_input_buffer()
p.rts = False

deadline = time.time() + 6
seen = bytearray()
while time.time() < deadline:
    chunk = p.read(256)
    if chunk:
        seen += chunk
        if b"tos>" in seen or b"shell" in seen.lower():
            break
time.sleep(0.3)
p.reset_input_buffer()

for c in CMDS:
    p.write((c + "\n").encode())
    time.sleep(1.2)
    out = bytearray()
    t0 = time.time()
    while time.time() - t0 < 1.0:
        chunk = p.read(256)
        if chunk:
            out += chunk
            t0 = time.time()
    print(f"--- {c}")
    print(out.decode("utf-8", "replace"))
p.close()
