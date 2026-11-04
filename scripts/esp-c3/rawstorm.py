#!/usr/bin/env python3
"""How much of a burst survives while the raw console reader is away?

user/rawsink.c sleeps through a burst of a known size and then drains what is
left, so the number it reports is the whole buffer behind the reader.  That
turns out to be the raw ring alone - the 128-byte hardware FIFO contributes
nothing, because the receive interrupt empties it into the ring as it goes -
and losing one byte to the full/empty distinction makes the answer RAW_Q - 1.
Nothing here is derived from the source, which is the point: two kernel builds
produce two numbers and the difference is the ring change.

    RAW_Q 512  -> 511 bytes survived
    RAW_Q 1024 -> 1023 bytes survived

A burst has to be bigger than the buffer and has to finish before the reader
wakes up, or the reader keeps up and nothing is ever lost:

    stall >= burst / 11.52 ms          (4096 bytes = 356 ms of wire)

    python3 scripts/esp-c3/rawstorm.py COM4
    python3 scripts/esp-c3/rawstorm.py COM4 --stalls 500 --burst 8192 --repeat 3
"""
import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

BAUD = 115200
BYTES_PER_MS = BAUD / 10.0 / 1000.0     # 8N1: 11.52 B/ms

SEQ_LO, SEQ_HI = 0x20, 0x7E


def pattern(n):
    span = SEQ_HI - SEQ_LO + 1
    return bytes(SEQ_LO + (i % span) for i in range(n))


def drain(ser, quiet=0.25, limit=1.0):
    buf = bytearray()
    t0 = time.time()
    last = t0
    while time.time() - t0 < limit:
        c = ser.read(4096)
        if c:
            buf += c
            last = time.time()
        elif time.time() - last > quiet:
            break
    return bytes(buf)


def reset_board(ser):
    time.sleep(0.2)
    ser.dtr = False
    ser.rts = True
    time.sleep(0.15)
    ser.reset_input_buffer()
    ser.rts = False
    time.sleep(0.2)
    end = time.time() + 6
    while time.time() < end:
        ser.read(4096)
    time.sleep(0.3)
    ser.write(b"\n")
    drain(ser, 0.3, 3.0)


def wait_for(ser, token, timeout):
    buf = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        c = ser.read(256)
        if c:
            buf += c
            if token.encode() in bytes(buf):
                return bytes(buf)
    return bytes(buf)


def trial(ser, stall, burst):
    # Clean slate: drop raw mode if a previous run left it on, and any
    # half-typed line the shell is holding.
    ser.write(b"\x03")
    time.sleep(0.05)
    ser.write(b"\n")
    drain(ser, 0.3, 2.0)

    ser.write(("rawsink %d %d\n" % (stall, burst)).encode())
    got = wait_for(ser, "READY", 5.0)
    if b"READY" not in got:
        return {"stall": stall, "status": "no-start",
                "tail": got[-80:].decode("utf-8", "backslashreplace")}

    # Straight into the UART driver: it queues the whole burst and meters it
    # out at the wire rate, which is exactly the pressure the ring sees.
    ser.write(pattern(burst))

    # Wake the reader only after its nap is over, so the stop byte cannot
    # occupy a slot that burst data would otherwise have taken.
    time.sleep(stall / 1000.0 + 0.15)
    ser.write(b"\n" * 4)

    reply = bytearray()
    end = time.time() + 6.0
    while time.time() < end:
        c = ser.read(4096)
        if c:
            reply += c
            if b"rawsink:" in bytes(reply):
                break
    text = bytes(reply).decode("utf-8", "backslashreplace")
    m = re.search(r"rawsink: stall (\d+) ms\s+want (\d+)\s+reads (\d+)\s+"
                  r"got (\d+)\s+gaps (\d+)\s+lost (\d+)\s+worst (\d+)", text)
    if not m:
        return {"stall": stall, "status": "no-report", "tail": text[-140:]}
    return {
        "stall": int(m.group(1)),
        "status": "ok",
        "want": int(m.group(2)),
        "reads": int(m.group(3)),
        "got": int(m.group(4)),
        "gaps": int(m.group(5)),
        "lost": int(m.group(6)),
        "worst": int(m.group(7)),
        "burst": burst,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default="COM4")
    ap.add_argument("--stalls", default="500,800",
                    help="reader nap times, ms (must exceed burst wire time)")
    ap.add_argument("--burst", type=int, default=4096)
    ap.add_argument("--repeat", type=int, default=2)
    ap.add_argument("--no-reset", action="store_true")
    args = ap.parse_args()

    wire_ms = args.burst / BYTES_PER_MS
    stalls = [int(s) for s in args.stalls.split(",") if s.strip()]
    short = [s for s in stalls if s < wire_ms]
    if short:
        print("note: stall %s ms is shorter than the %.0f ms the burst takes "
              "to reach the wire - the reader will keep up and show no loss"
              % (short, wire_ms))

    ser = serial.Serial(args.port, BAUD, timeout=0.05, write_timeout=20)
    if not args.no_reset:
        reset_board(ser)

    results = {}
    try:
        for st in stalls:
            vals = []
            for _ in range(args.repeat):
                r = trial(ser, st, args.burst)
                if r["status"] != "ok":
                    print("stall %4d ms  %s  %r" % (st, r["status"], r.get("tail")))
                    continue
                vals.append(r["got"])
                print("stall %4d ms  burst %5d  got %5d  reads %3d  gaps %3d  lost %5d  worst %3d"
                      % (st, r["burst"], r["got"], r["reads"], r["gaps"],
                         r["lost"], r["worst"]))
            if vals:
                results[st] = max(vals)
    finally:
        ser.close()

    print()
    for st in sorted(results):
        print("stall %4d ms -> survived %5d bytes  (%d bytes of wire, %.1f ms)"
              % (st, results[st], results[st],
                 results[st] / BYTES_PER_MS))
    if results:
        print("\nbuffer behind the reader = %d bytes  (raw ring; the hardware "
              "FIFO does not add to it)" % max(results.values()))


if __name__ == "__main__":
    main()
