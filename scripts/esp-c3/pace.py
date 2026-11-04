#!/usr/bin/env python3
"""Where do the bytes of a transfer line get lost, and does pacing help?

Sends one D line for a known payload and reverses the damage: the NAK reply
echoes the first 20 characters rx actually saw, so searching that fragment in
the base64 the host sent says exactly how many bytes never made it.  The
number identifies the layer:

    ~128                the hardware receive FIFO (128 deep) overran, i.e. the
                        kernel did not read the port for >11 ms
    ~RAW_Q (1024/2048)  the software raw ring filled up, i.e. the reader was
                        not scheduled while the wire kept delivering
    other               a protocol/logic problem, not a buffer one

Options --piece/--gap re-send the same line with real idle time between
pieces (the gap must exceed the transmit time of a piece to be a gap at all:
115200 baud is 11.5 bytes/ms).

    python3 scripts/esp-c3/pace.py COM4
    python3 scripts/esp-c3/pace.py COM4 --piece 32 --gap 0.02
"""
import argparse
import base64
import os
import random
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")


def b64_raw(data):
    return base64.b64encode(data).decode().rstrip("=")


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


def reset_board(pulse=True, wait_boot=True):
    ser = serial.Serial(PORT, 115200, timeout=0.05)
    time.sleep(0.2)
    if pulse:
        ser.dtr = False
        ser.rts = True
        time.sleep(0.15)
        ser.reset_input_buffer()
        ser.rts = False
        time.sleep(0.2)
        end = time.time() + 6
        while time.time() < end:
            ser.read(4096)
    if wait_boot:
        time.sleep(0.3)
        ser.write(b"\n")
        drain(ser, 0.3, 3.0)
    return ser


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


def trial(ser, name, payload, piece, gap):
    line = "D 0 %d %s\n" % (len(payload), b64_raw(payload))
    raw = line.encode()

    # Fresh prompt: ^C in case a previous rx is still reading, then a newline.
    ser.write(b"\x03")
    time.sleep(0.05)
    ser.write(b"\n")
    drain(ser, 0.3, 2.0)

    ser.write(("rx /%s\n" % name).encode())
    got = wait_for(ser, "READY", 5.0)
    if b"READY" not in got:
        return {"name": name, "status": "no-reader", "reply": got[-60:]}

    t0 = time.time()
    if piece <= 0:
        ser.write(raw)
    else:
        for off in range(0, len(raw), piece):
            ser.write(raw[off:off + piece])
            if off + piece < len(raw):
                time.sleep(gap)
    t_sent = time.time()

    reply = bytearray()
    end = time.time() + 6.0
    while time.time() < end:
        c = ser.read(512)
        if c:
            reply += c
            if b"\n" in reply and (b"ACK 0" in reply or b"NAK" in reply
                                   or b"ERR" in reply or b"DONE" in reply):
                # a NAK may be followed by another line on a retry; take the first
                break
    reply = bytes(reply)
    t_reply = time.time()
    return {
        "name": name,
        "status": "replied",
        "reply": reply.decode("utf-8", "backslashreplace").strip(),
        "send_s": t_sent - t0,
        "wait_s": t_reply - t_sent,
    }


def locate(payload, reply):
    """How many bytes of the line the host sent never reached rx."""
    b64 = b64_raw(payload)
    head = "D 0 %d " % len(payload)
    start = reply.find("r1[")
    if start < 0:
        return None
    frag = reply[start + 3:].split("]")[0]
    if len(frag) < 8:
        return None
    idx = b64.find(frag[:12])
    if idx < 0:
        return "fragment not from this line: %r" % frag
    return idx + len(head)


def main():
    global PORT
    ap = argparse.ArgumentParser()
    ap.add_argument("port", nargs="?", default="COM4")
    ap.add_argument("--piece", type=int, default=0,
                    help="bytes per write, 0 = one single write")
    ap.add_argument("--gap", type=float, default=0.0,
                    help="seconds of idle time between pieces")
    ap.add_argument("--size", type=int, default=1024)
    ap.add_argument("--no-reset", action="store_true")
    ap.add_argument("--stats", action="store_true",
                    help="after the trial, ^C the reader and dump console")
    args = ap.parse_args()
    PORT = args.port

    rng = random.Random(1234)
    payload = bytes(rng.getrandbits(8) for _ in range(args.size))
    ser = reset_board(pulse=not args.no_reset)
    try:
        tag = int(time.time()) % 100000
        r = trial(ser, "p%d.bin" % tag, payload, args.piece, args.gap)
        print(r)
        if r.get("reply"):
            lost = locate(payload, r["reply"])
            print("line bytes = %d, lost at offset = %r" % (len(payload) * 4 // 3 + 12, lost))
        if args.stats:
            # Same session: closing the port pulses EN and reboots the board,
            # which would wipe the very counters we want to read.
            ser.write(b"\x03")
            time.sleep(0.4)
            tail = bytearray()
            end = time.time() + 3.0
            while time.time() < end:
                c = ser.read(4096)
                if c:
                    tail += c
            print("--- console after ^C ---")
            print(tail.decode("utf-8", "backslashreplace"))
    finally:
        ser.close()


if __name__ == "__main__":
    main()
