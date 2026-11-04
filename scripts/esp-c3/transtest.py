#!/usr/bin/env python3
"""Regression test for the console file-transfer path (rx + the usj console).

Why this exists
---------------
The native USB-Serial/JTAG console (usj) used to block with interrupts off
while waiting for a USB host: with nothing plugged in, every console byte spun
~40000 iterations, and because the kernel prints from inside a trap (interrupts
disabled) that starved the UART0 receive FIFO and truncated incoming file
transfers.  The symptom was "tosput stuck at chunk 0" -- see docs/OPEN-ITEMS.md
item 10 and docs/FILE-TRANSFER.md.  The fix made the console never block on
USB (a 1 KiB ring drained by the SERIAL_IN_EMPTY interrupt, dropping like a full
UART transmitter when no host reads).

This test is the guard against that regressing: it speaks the rx protocol over
the wired console (COM4 / UART0) and checks the board-computed CRC32 of every
transferred file against the host's.  Crucially, even though the bytes arrive
over UART0, *every* board-side print (READY / ACK / DONE) is also pushed through
usj_putc(); so if usj_putc ever goes back to blocking-with-IRQs-off, the UART0
receiver is starved mid-transfer and the CRC check fails.  A large transfer
exercises a sustained ACK storm; a 1-chunk file reproduces the original
chunk-0 stall.  Either way the regression is caught.

It is a self-contained third implementation of the protocol (the others are
tools/tosput/sender.go on the host and user/rx.c on the board), which also
cross-checks both of those.

    scripts/esp-c3/transtest.py COM4
    scripts/esp-c3/transtest.py COM4 --sizes 1024,16384,65536
    scripts/esp-c3/transtest.py COM4 --no-reset      # board already booted

The board must be flashed with a TinyOS image that contains bin/rx in its
littlefs ramdisk (rebuild with: go run ./cmd/tos build -target esp32c3).

Protocol mirror of tools/tosput/sender.go + user/rx.c
    host: "rx <path>\\n"           -> board "READY <chunk>\\n"  (or "ERR ...")
    host: "D <seq> <len> <b64>\\n" -> board "ACK <seq>\\n" / "NAK <seq> ..\\n"
    host: "E <crc32hex>\\n"        -> board "DONE <crc32hex> <total>\\n"
Chunks are 1024 bytes, base64 raw (unpadded).  Stop-and-wait: the next chunk is
not sent until the previous ACK lands, because the board is inside write()
programming flash and cannot read while that happens.
"""

import argparse
import base64
import os
import re
import sys
import time
import zlib

import serial

PROMPT = b"tos> "
CHUNK = 1024
LINE_TIMEOUT = 6.0       # per-line read budget
ACK_WAIT = 6.0           # time to wait for an ACK after a chunk
READY_WAIT = 12.0        # rx may create + the fs self-test on first use
RETRIES = 4              # chunk retransmits on NAK / lost ACK


def b64_raw(b):
    # Raw (unpadded) base64, exactly what rx.c decodes and what
    # base64.RawStdEncoding produces on the Go side.
    return base64.b64encode(b).rstrip(b"=").decode("ascii")


class Board:
    def __init__(self, port, baud=115200):
        self.s = serial.Serial(port, baud, timeout=0.1)
        self.s.dtr = False
        self.s.rts = False

    def reset(self, settle=0.3):
        # RTS pulse == EN on the devkit's auto-download circuit.
        self.s.rts = True
        time.sleep(0.15)
        self.s.reset_input_buffer()
        self.s.rts = False
        time.sleep(settle)

    def close(self):
        self.s.close()

    def read_line(self, timeout):
        """Return one \\n-terminated line (newline stripped), or None on
        timeout.  CRLF is normalised to LF by the line discipline."""
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            c = self.s.read(1)
            if c:
                buf += c
                if c[0] == 0x0A:
                    return buf.decode("utf-8", "replace").rstrip("\r\n")
        return None

    def read_until_prompt(self, timeout=20.0):
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.s.read(512)
            if chunk and buf.rstrip().endswith(PROMPT):
                break
            buf += chunk
        return buf.decode("utf-8", "replace")

    def write_paced(self, data, piece=256, gap=0.001):
        """Send a protocol line in small writes with tiny gaps.  Mirrors
        sender.go: an unbroken burst longer than the 128-byte RX FIFO used to
        overrun before the interrupt-driven drain landed."""
        for off in range(0, len(data), piece):
            end = min(off + piece, len(data))
            self.s.write(data[off:end])
            if end < len(data):
                time.sleep(gap)

    def expect(self, pred, timeout):
        """Read lines until pred(line) is true, or return None on timeout."""
        end = time.time() + timeout
        while time.time() < end:
            line = self.read_line(0.5)
            if line is None:
                continue
            if pred(line):
                return line
        return None

    def transfer(self, data, remote):
        """Run the rx protocol for `data`, returning (ok, detail)."""
        crc = zlib.crc32(data) & 0xFFFFFFFF
        total = len(data)

        self.s.write(("rx %s\n" % remote).encode())
        line = self.expect(lambda l: l.startswith("READY") or l.startswith("ERR"),
                           READY_WAIT)
        if line is None:
            return False, "no READY from rx (is bin/rx in the image?)"
        if line.startswith("ERR"):
            return False, "board: " + line
        # READY carries the board's chunk size: the kernel and the user
        # programs are flashed separately, so a stale filesystem image is a
        # real possibility and it fails exactly like lost bytes (line
        # truncated at the old LINE_MAX, NAK r1 mid-base64).  Check it here
        # so the failure names the cause instead of looking like a UART bug.
        fields = line.split()
        if len(fields) < 2:
            return False, "board's rx does not report its chunk size: " \
                          "stale filesystem image, flash without --keep-fs"
        if fields[1] != str(CHUNK):
            return False, "board's rx speaks %s-byte chunks, host %d: " \
                          "stale filesystem image, flash without --keep-fs" \
                          % (fields[1], CHUNK)

        for seq in range(0, (total + CHUNK - 1) // CHUNK):
            block = data[seq * CHUNK:(seq + 1) * CHUNK]
            payload = ("D %d %d %s\n" % (seq, len(block), b64_raw(block))).encode()
            ok = False
            last = ""
            for _ in range(RETRIES):
                self.write_paced(payload)
                resp = self.expect(
                    lambda l, s=seq: l == ("ACK %d" % s)
                    or l.startswith("NAK") or l.startswith("ERR"),
                    ACK_WAIT)
                if resp is None:
                    last = "timeout waiting for ACK %d" % seq
                    continue
                if resp == ("ACK %d" % seq):
                    ok = True
                    break
                last = resp  # NAK / ERR -> retransmit same chunk
            if not ok:
                return False, "chunk %d: %s" % (seq, last)

        self.s.write(("E %08x\n" % crc).encode())
        done = self.expect(lambda l: l.startswith("DONE") or l.startswith("ERR"),
                           ACK_WAIT)
        if done is None:
            return False, "no DONE from rx"
        if done.startswith("ERR"):
            return False, "board: " + done
        m = re.match(r"DONE ([0-9a-fA-F]{8}) (\d+)", done)
        if not m:
            return False, "bad DONE line: %r" % done
        got_crc = int(m.group(1), 16)
        got_total = int(m.group(2))
        if got_crc != crc:
            return False, "crc mismatch board=%08x host=%08x" % (got_crc, crc)
        if got_total != total:
            return False, "size mismatch board=%d host=%d" % (got_total, total)
        return True, "crc32 %08x, %d bytes" % (crc, total)


def main():
    ap = argparse.ArgumentParser(description="esp32c3 console file-transfer regression test")
    ap.add_argument("port", nargs="?", default="COM4")
    ap.add_argument("--sizes", default="1024,16384,65536",
                    help="comma-separated file sizes in bytes to transfer")
    ap.add_argument("--remote", default="/transtest.bin",
                    help="remote path the board writes to")
    ap.add_argument("--no-reset", action="store_true")
    args = ap.parse_args()

    try:
        sizes = [int(x) for x in args.sizes.split(",") if x.strip()]
    except ValueError:
        print("bad --sizes: %r" % args.sizes, file=sys.stderr)
        return 2
    if not sizes:
        sizes = [1024]

    b = Board(args.port)
    if not args.no_reset:
        b.reset()
        boot = b.read_until_prompt(25.0)
        if PROMPT.decode() not in boot:
            print("FAIL  board did not reach a shell prompt after reset")
            print("       --- boot tail ---")
            for line in boot.splitlines()[-15:]:
                print("       " + line)
            b.close()
            return 1

    # Confirm bin/rx is in the image before blaming the transfer path.
    b.s.write(b"ls bin\n")
    ls = b.read_until_prompt(10.0)
    if "rx" not in ls:
        print("FAIL  bin/rx not found in the ramdisk image")
        print("       rebuild with: go run ./cmd/tos build -target esp32c3")
        print("       --- ls bin ---")
        for line in ls.splitlines():
            if line.strip():
                print("       " + line)
        b.close()
        return 1

    results = []
    for size in sizes:
        data = os.urandom(size)
        t0 = time.time()
        ok, detail = b.transfer(data, args.remote)
        dt = time.time() - t0
        kibs = (size / 1024.0) / dt if dt > 0 else 0.0
        tag = "PASS" if ok else "FAIL"
        print("%s  %7d bytes  %.1f KiB/s  %s" % (tag, size, kibs, detail))
        results.append((size, ok, detail))

    # Tidy up the scratch file (best effort; do not fail on it).
    b.s.write(("rm %s\n" % args.remote).encode())
    b.read_until_prompt(5.0)
    b.close()

    failed = [r for r in results if not r[1]]
    print("\n%d/%d transfer(s) verified" % (len(results) - len(failed), len(results)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
