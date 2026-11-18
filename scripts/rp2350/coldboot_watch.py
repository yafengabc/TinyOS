"""Watch a cold boot: report whether the board shows a UF2 volume, whether the
console emits anything, and in what order.  Run this, then power-cycle the
board.

    python scripts/rp2350/coldboot_watch.py -p COM19 [seconds]
"""

import os
import string
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else 'COM19'
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 40.0


def bootsel():
    for c in string.ascii_uppercase:
        root = c + ':/'
        if os.path.isfile(root + 'INFO_UF2.TXT'):
            return root
    return None


def main():
    s = None
    for _ in range(10):
        try:
            s = serial.Serial(PORT, 115200, timeout=0.05)
            break
        except Exception:
            time.sleep(0.5)
    if s:
        s.dtr = False
        s.rts = False
        s.reset_input_buffer()

    t0 = time.time()
    events = []
    drive_seen = None
    total = 0
    last_drive = None
    while time.time() - t0 < SECONDS:
        now = time.time() - t0
        d = bootsel()
        if d != last_drive:
            events.append('%.2fs  UF2 volume %s' % (now, d if d else 'gone'))
            last_drive = d
        if d and not drive_seen:
            drive_seen = now
        if s:
            chunk = s.read(4096)
            if chunk:
                total += len(chunk)
                events.append('%.2fs  uart +%d bytes  %r' % (now, len(chunk), bytes(chunk)[:60]))
        time.sleep(0.05)
    if s:
        s.close()

    print('\n'.join(events) if events else 'no events at all')
    print('\nsummary: uf2 volume %s, %d uart bytes'
          % ('appeared' if drive_seen is not None else 'never', total))
    return 0


if __name__ == '__main__':
    sys.exit(main())
