"""Flash one image over BOOTSEL, wait for the shell, then run the six-item
heartbeat on it.  Used for the Pico 2 ISA/SMP matrix; the board must be
presented to the host as the RP2350 UF2 volume before this is started.

    python scripts/rp2350/flash_heartbeat.py --uf2 build/tinyos-rpi-pico2-smp.uf2 \
        -p COM19 --target rpi-pico2-smp
"""

import argparse
import os
import re
import string
import sys
import time

import serial


def find_bootsel():
    for c in string.ascii_uppercase:
        root = c + ':/'
        if os.path.isfile(root + 'INFO_UF2.TXT'):
            return root
    return None


def wait_prompt(s, timeout):
    buf = bytearray()
    end = time.time() + timeout
    while time.time() < end:
        chunk = s.read(4096)
        if chunk:
            buf += chunk
            if buf.decode('utf-8', 'replace').rstrip().endswith('tos>'):
                break
    return buf.decode('utf-8', 'replace')


def cmd(s, line, timeout=10.0):
    s.write(line.encode() + b'\r\n')
    return wait_prompt(s, timeout)


def uptime_seconds(text):
    m = re.search(r'uptime:\s*([0-9]+(?:\.[0-9]+)?)\s*s', text)
    return float(m.group(1)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--uf2', required=True)
    ap.add_argument('-p', '--port', required=True)
    ap.add_argument('--target', required=True)
    ap.add_argument('--boot-timeout', type=float, default=25.0)
    args = ap.parse_args()

    drv = find_bootsel()
    if not drv:
        print('NO_BOOTSEL: press BOOTSEL first')
        return 2
    print('bootsel %s, writing %s' % (drv, os.path.basename(args.uf2)))

    s = serial.Serial(args.port, 115200, timeout=0.1)
    s.dtr = False
    s.rts = False
    s.reset_input_buffer()

    with open(args.uf2, 'rb') as f:
        data = f.read()
    with open(drv + 'fw.uf2', 'wb') as f:
        f.write(data)
    t0 = time.time()
    while time.time() - t0 < 30 and os.path.isfile(drv + 'INFO_UF2.TXT'):
        s.read(4096)
        time.sleep(0.05)
    print('ejected after %.1fs' % (time.time() - t0))

    boot = wait_prompt(s, args.boot_timeout)
    if 'tos>' not in boot:
        # The ROM on this clone does not always restart the image by itself.
        s.write(b'\r')
        time.sleep(0.4)
        boot += wait_prompt(s, 3.0)
    print('--- boot ---')
    print(boot.strip()[:700])
    print('------------')

    results = []

    def check(name, ok, detail=''):
        results.append(bool(ok))
        print('%s  %-26s  %s' % ('PASS' if ok else 'FAIL', name, detail))

    check('1 boot to a shell', 'tos>' in boot, 'prompt seen')

    u1 = uptime_seconds(cmd(s, 'uptime'))
    t1 = time.monotonic()
    time.sleep(1.0)
    u2 = uptime_seconds(cmd(s, 'uptime'))
    wall = time.monotonic() - t1
    delta = (u2 - u1) if (u1 is not None and u2 is not None) else None
    check('2 time advances 1 s',
          delta is not None and abs(delta - wall) <= 0.1,
          'delta=%.3f wall=%.3f' % (delta, wall) if delta is not None else 'no uptime')

    check('3 filesystem read', bool(re.search(r'\bmsh\b', cmd(s, 'ls /bin'))), 'ls /bin')
    check('4 user program', 'hello from ring 3!' in cmd(s, 'hello'), 'hello')

    cmd(s, 'wfile hb.tmp x')
    cat = cmd(s, 'cat hb.tmp')
    rm = cmd(s, 'rm hb.tmp')
    check('5 filesystem write-back', 'x' in cat and 'tos>' in rm, 'wfile/cat/rm')

    crash = cmd(s, 'crash')
    check('6 fault isolation', 'tos>' in crash and 'KERNEL PANIC' not in crash,
          'crash returned to prompt')

    ps = cmd(s, 'ps')
    print('--- ps ---')
    print(ps.strip()[:400])

    s.close()
    passed = sum(1 for r in results if r)
    print('\n%s %d/%d  target=%s port=%s'
          % ('HEARTBEAT OK' if passed == len(results) else 'HEARTBEAT FAIL',
             passed, len(results), args.target, args.port))
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
