#!/usr/bin/env python3
"""dd/rm stress for the RP2350 port (the flash write path, not just reads).

Each round writes 32 KiB of zeros, reads it back (the shell prints
"pattern verified"), and removes it.  A round passes only if the read-back
verifies; any failed round is reported with the console output that produced
it, because on this port a bad flash write shows up as a wrong pattern, a
write error, or a wedged board - and all three need to be distinguishable.

    python scripts/rp2350/dd_stress.py -p COM19 --target rpi-pico2 --rounds 1000

The board must already be running the image.  Exit code 0 only when every
round verified; the final line is `DD OK n/N` or `DD FAIL n/N`.
"""

import argparse
import sys
import time

import serial


class Console:
    def __init__(self, port, dtr=False):
        self.s = serial.Serial(port, 115200, timeout=0.1)
        self.s.dtr = bool(dtr)
        self.s.reset_input_buffer()

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def drain(self, seconds):
        buf = bytearray()
        end = time.time() + seconds
        while time.time() < end:
            chunk = self.s.read(4096)
            if chunk:
                buf += chunk
        return buf.decode('utf-8', 'replace')

    def prompt(self, timeout):
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.s.read(4096)
            if chunk:
                buf += chunk
                if buf.decode('utf-8', 'replace').rstrip().endswith('tos>'):
                    break
        return buf.decode('utf-8', 'replace')

    def cmd(self, line, timeout=30.0):
        self.s.write(line.encode() + b'\r\n')
        return self.prompt(timeout)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-p', '--port', required=True)
    ap.add_argument('--target', default='rpi-pico2')
    ap.add_argument('--dtr', action='store_true', help='assert DTR (USB CDC console)')
    ap.add_argument('--rounds', type=int, default=1000)
    ap.add_argument('--blocks', type=int, default=8, help='4 KiB blocks per file')
    ap.add_argument('--keep', action='store_true', help='do not rm between rounds')
    args = ap.parse_args()

    try:
        con = Console(args.port, dtr=args.dtr)
    except serial.SerialException as e:
        print('cannot open %s: %s' % (args.port, e))
        return 2

    size = args.blocks * 4096
    print('dd stress: %d rounds, %d KiB per round, target %s, port %s'
          % (args.rounds, size // 1024, args.target, args.port))

    ok = 0
    bad_streak = 0
    t0 = time.time()
    try:
        # A prompt first: a board that is not at the shell must fail here, not
        # silently after 1000 rounds of nothing.
        if 'tos>' not in con.cmd('', 10.0):
            print('FAIL  no shell prompt on %s' % args.port)
            return 1

        for i in range(args.rounds):
            out = con.cmd('dd if=/dev/zero of=/x bs=4096 count=%d' % args.blocks)
            out += con.cmd('dd if=/x bs=4096 count=%d' % args.blocks)
            if not args.keep:
                out += con.cmd('rm /x', 20.0)
            good = 'pattern verified' in out
            if good:
                ok += 1
                bad_streak = 0
            else:
                bad_streak += 1
                print('FAIL  round %d/%d' % (i + 1, args.rounds))
                print(out.strip()[:600])
                sys.stdout.flush()
                # Three dead rounds in a row is a wedged board, not a bad
                # pattern: stop instead of spending the rest of the hour on
                # 30 s timeouts.
                if bad_streak >= 3:
                    print('stop: %d consecutive failed rounds' % bad_streak)
                    break
            if (i + 1) % 10 == 0 or i + 1 == args.rounds:
                rate = (i + 1) / max(time.time() - t0, 1e-6)
                eta = (args.rounds - i - 1) / max(rate, 1e-6)
                print('  round %d/%d  ok=%d  %.1f rounds/s  eta %.0fs'
                      % (i + 1, args.rounds, ok, rate, eta))
                sys.stdout.flush()
    finally:
        con.close()

    print('\n%s %d/%d  target=%s port=%s'
          % ('DD OK' if ok == args.rounds else 'DD FAIL', ok, args.rounds,
             args.target, args.port))
    return 0 if ok == args.rounds else 1


if __name__ == '__main__':
    sys.exit(main())
