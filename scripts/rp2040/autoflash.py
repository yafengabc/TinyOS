#!/usr/bin/env python3
"""RP2040 BOOTSEL auto-flash watcher.

On this board BOOTSEL can only be entered by a physical button press: the
CH343 DTR/RTS lines are not wired to BOOTSEL/RESET (verified with four
different signalling sequences).  Keep this script running and one button
press becomes one complete "flash + capture boot log" cycle -- no need for
a round trip through the assistant.

Typical use:

    # one shot: press BOOTSEL, script flashes as soon as F: appears
    python scripts/rp2040/autoflash.py --uf2 build/rp2040.uf2

    # resident: every future button press flashes the same image
    python scripts/rp2040/autoflash.py --uf2 build/rp2040.uf2 --watch

    # resident with a queue: each press flashes the next image in order
    python scripts/rp2040/autoflash.py --uf2 a.uf2 --uf2 b.uf2 --watch

The UART port is opened BEFORE the UF2 copy so no boot characters are lost.
Each round writes a timestamped transcript to build/rp2040-autoflash/.
"""

import argparse
import os
import shutil
import string
import sys
import time
from datetime import datetime

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")


def bootsel_drive():
    """Return the drive root of a mounted RP2040 BOOTSEL volume, or None."""
    for letter in string.ascii_uppercase:
        root = letter + ":/"
        if os.path.isfile(root + "INFO_UF2.TXT"):
            return root
    return None


def serial_ports():
    return {p.device: p.description for p in list_ports.comports()}


def wait_for_drive(timeout, log):
    """Wait for a BOOTSEL volume.  timeout=None waits forever."""
    t0 = time.time()
    while True:
        drv = bootsel_drive()
        if drv:
            return drv
        if timeout is not None and time.time() - t0 > timeout:
            return None
        if int(time.time() - t0) % 10 == 0:
            log("waiting for BOOTSEL drive ... %.0fs" % (time.time() - t0))
        time.sleep(0.2)


def wait_drive_gone(drv, timeout, log):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not bootsel_drive():
            return time.time() - t0
        time.sleep(0.15)
    log("WARNING: drive %s still present after %.1fs" % (drv, timeout))
    return None


def grab(port, seconds):
    """Read bytes for `seconds`, also recording when bursts arrived."""
    t0 = time.time()
    out = bytearray()
    marks = []
    while time.time() - t0 < seconds:
        n = port.in_waiting
        if n:
            out += port.read(n)
            marks.append(round(time.time() - t0, 2))
        else:
            time.sleep(0.005)
    return bytes(out), marks


def read_until_prompt(port, prompt=b"tos> ", timeout=6.0):
    """Shell replies can be long and slow; wait for the next prompt instead
    of a fixed window, otherwise output leaks into the next command."""
    t0 = time.time()
    out = bytearray()
    while time.time() - t0 < timeout:
        n = port.in_waiting
        if n:
            out += port.read(n)
            if prompt in bytes(out):
                time.sleep(0.06)
                if port.in_waiting:
                    out += port.read(port.in_waiting)
                break
        else:
            time.sleep(0.005)
    return bytes(out)


# "ls /bin" is deliberately absent: on this 12 MHz port its long listing
# takes longer than the prompt wait and leaks into every following command's
# transcript, which looks like a shell hang but is just a slow external
# program.  Keep the acceptance set short and individually prompt-terminated.
ACCEPT_COMMANDS = (
    "uptime", "ls /", "ps", "free", "df", "echo HELLO_RP2040",
    "mem", "wheap", "cat /proc/cpuinfo",
    # ADC bring-up, wc's file argument, the board device table, and PWM.
    # GP25 is the on-board LED, so a 50% setting is visible.
    "adc", "wc readme.txt", "cat /proc/dev",
    "pwm", "pwm 25 1000 50", "pwm 25 off",
    "uart", "uart send TINYOS-UART1", "uart recv 100",
    "spi", "spi loopback 0 1000000",
    "i2c", "i2c scan 0 100000",
    "dma",
)

TAGS = (
    b"[rp2040] boot", b"clk=125MHz", b"[console] uart0 primary", b"[usb] init done",
    b"[p] board init", b"[p] rom lookup", b"[p] timer init",
    b"[p] bd read", b"[p] bd erase", b"[p] bd write",
    b"[p] console putc", b"[p] tick unmask", b"[p] first tick",
    b"tick self-check", b"TinyOS", b"tos> ", b"[bootsel] entering ROM",
    b"ramdisk", b"KERNEL PANIC", b"flash writes enabled",
)


def run_round(args, uf2, drv, round_no, transcript):
    def log(msg):
        line = "[round %d %s] %s" % (round_no, datetime.now().strftime("%H:%M:%S"), msg)
        print(line, flush=True)
        transcript.append(line)

    if not os.path.isfile(uf2):
        log("ABORT: firmware missing: %s" % uf2)
        return False

    before = serial_ports()
    log("drive=%s ports_before=%s" % (drv, sorted(before)))

    # Attach the UART before copying: the board reboots the instant the UF2
    # write completes, and early boot characters would otherwise be lost.
    # The CH343 re-enumerates at the exact moment BOOTSEL appears, so a
    # single open() often loses a PermissionError race.  Retry briefly: the
    # UF2 volume stays mounted, so there is no deadline to miss.
    c = None
    for attempt in range(12):
        try:
            c = serial.Serial(args.port, args.baud, timeout=0.02)
            break
        except Exception as e:
            err = e
            time.sleep(0.25)
    if c is None:
        # The CH343 can disappear while the board is in the ROM BOOTSEL
        # volume; the UF2 copy itself needs no serial port, so continue and
        # attach after the reboot instead of aborting the flash.
        log("NOTE: %s not present before flash (%s); will attach after reboot"
            % (args.port, err))
    else:
        c.dtr = False
        c.rts = False
        c.reset_input_buffer()
        log("%s @%d attached before flash (try %d)" %
            (args.port, args.baud, attempt + 1))

    t0 = time.time()
    try:
        shutil.copyfile(uf2, drv + os.path.basename(uf2))
    except Exception as e:
        log("ABORT: UF2 copy failed: %s" % e)
        if c is not None:
            c.close()
        return False
    log("copied %s (%d bytes) in %.2fs" % (uf2, os.path.getsize(uf2), time.time() - t0))

    gone = wait_drive_gone(drv, args.eject_timeout, log)
    log("drive ejected after %s -> board rebooted" %
        ("%.2fs" % gone if gone is not None else "TIMEOUT"))

    if c is None:
        for attempt in range(120):
            try:
                c = serial.Serial(args.port, args.baud, timeout=0.02)
                break
            except Exception as e:
                err = e
                time.sleep(0.25)
        if c is None:
            log("ABORT: cannot open %s after reboot: %s" % (args.port, err))
            return False
        c.dtr = False
        c.rts = False
        c.reset_input_buffer()
        log("%s @%d attached after reboot (try %d)" %
            (args.port, args.baud, attempt + 1))

    data, marks = grab(c, args.grab)
    log("UART: %d bytes / %d bursts; burst times %s" % (len(data), len(marks), marks[:25]))
    transcript.append("---- decoded boot log ----")
    transcript.append(data.decode("utf-8", errors="replace"))
    log("---- decoded boot log ----")
    print(data.decode("utf-8", errors="replace")[:4000], flush=True)

    counts = [(t.decode(), data.count(t)) for t in TAGS if data.count(t)]
    log("tags: %s" % (", ".join("%s=%d" % kv for kv in counts) or "none"))
    boot_ports = sorted(serial_ports())
    log("ports after boot: %s | new: %s" %
        (boot_ports, [k for k in boot_ports if k not in before] or "none"))

    if args.probe:
        c.reset_input_buffer()
        c.write(b"\r\n")
        c.flush()
        time.sleep(0.2)
        c.write(b"uptime\r\n")
        c.flush()
        p, _ = grab(c, 1.5)
        log("after CR + uptime (%d bytes): %r" % (len(p), p[:300]))
        transcript.append("probe reply: %r" % p[:1000])

    if args.accept:
        c.reset_input_buffer()
        c.write(b"\r\n")
        c.flush()
        time.sleep(0.2)
        for cmd in ACCEPT_COMMANDS:
            c.reset_input_buffer()
            c.write((cmd + "\r\n").encode())
            c.flush()
            reply = read_until_prompt(c)
            text = reply.decode("utf-8", errors="replace")
            log("accept %-16s (%d bytes)" % (cmd, len(reply)))
            transcript.append("===== %s =====\n%s" % (cmd, text))
            print("===== %s =====\n%s" % (cmd, text[:800]), flush=True)

    if args.replug:
        # One firmware image, a full A/B without another button press:
        # Ctrl-R forces a fresh SE0->J while the kernel is fully running and
        # Ctrl-T prints the IRQ trace.  Every control byte is consumed by the
        # UART RX poller, not by the shell, so the console keeps working at
        # 115200 throughout.  (There used to be a Ctrl-P clk_peri switch
        # here; the runtime clock muxing is gone because it could kill the
        # console, so only the replug remains.)
        c.reset_input_buffer()
        c.write(b"\x12")
        c.flush()
        mid, _ = grab(c, 4.0)
        log("replug window (%d bytes): %r" % (len(mid), mid[:300]))
        log("ports after replug: %s" % sorted(serial_ports()))

        c.write(b"\x14")
        c.flush()
        post, _ = grab(c, 3.0)
        text = (mid + post).decode("utf-8", errors="replace")
        log("USB replug + trace: %d bytes" % len(mid + post))
        transcript.append("===== usb replug + trace =====\n" + text)
        print("===== usb replug + trace =====\n" + text[:5000], flush=True)

    if args.sysclk_ab:
        # USB A/B in ONE flash.  The runtime clk_sys switches are gone: the
        # port now boots on PLL_SYS at 125 MHz (same as pico-sdk/MicroPython),
        # and the old Ctrl-Y/Ctrl-U mux changes killed the console.  What is
        # still worth an A/B is erratum RP2040-E15: force a fresh SE0->J
        # replug with the workaround on, then off, and dump the IRQ trace.
        # Windows needs several seconds to retry a device that has just been
        # reset, so give every step a long capture window.
        scenarios = (
            ("125MHz/E15-on",  (b"\x05",)),   # Ctrl-E
            ("125MHz/E15-off", (b"\x17",)),   # Ctrl-W
        )
        for name, chars in scenarios:
            c.reset_input_buffer()
            for ch in chars:
                c.write(ch)
                c.flush()
                grab(c, 0.4)
            c.write(b"\x12")            # Ctrl-R: fresh replug
            c.flush()
            rp, _ = grab(c, 6.0)
            ports = sorted(serial_ports())
            new = [k for k in ports if k not in before]
            log("%s replug new ports: %s | all: %s" %
                (name, new or "none", ports))
            c.write(b"\x14")            # Ctrl-T: trace + TinyUSB log
            c.flush()
            tr, _ = grab(c, 2.0)
            text = (rp + tr).decode("utf-8", errors="replace")
            log("%s A/B: %d bytes" % (name, len(rp + tr)))
            transcript.append("===== %s =====\n%s" % (name, text))
            print("===== %s =====\n%s" % (name, text[:5000]), flush=True)

        # Leave the board with E15 on for the Ctrl-D test.
        c.write(b"\x05")
        c.flush()
        grab(c, 0.4)

    if args.ctrl_d:
        # Never measure Ctrl-D on a board whose console went away: last round
        # the clock A/B killed it and "NO RESPONSE" was a false negative.
        c.reset_input_buffer()
        c.write(b"\r\nuptime\r\n")
        c.flush()
        probe = read_until_prompt(c, timeout=2.0)
        log("Ctrl-D preflight console: %r" % probe[:120])
        if b"tos> " not in probe:
            log("Ctrl-D SKIPPED: console not alive; result would be a false negative")
        else:
            t = time.time()
            hit = None
            extra = bytearray()
            c.write(b"\x04")
            c.flush()
            while time.time() - t < args.ctrl_d_secs:
                n = c.in_waiting
                if n:
                    extra += c.read(n)
                if bootsel_drive():
                    hit = round(time.time() - t, 2)
                    break
                time.sleep(0.05)
            log("Ctrl-D -> BOOTSEL %s; extra bytes=%r" %
                ("after %.2fs" % hit if hit else "NO RESPONSE", bytes(extra[:160])))

    c.close()
    time.sleep(0.8)
    after = serial_ports()
    new = [k for k in after if k not in before]
    log("new ports: %s | all: %s" % (new or "none", sorted(after)))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--uf2", action="append", required=True,
                    help="UF2 image; repeat to build a queue (one per button press)")
    ap.add_argument("--port", default="COM19", help="UART0 serial port (default COM19)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--grab", type=float, default=10.0, help="seconds of boot log to capture")
    ap.add_argument("--watch", action="store_true", help="keep watching for the next BOOTSEL press")
    ap.add_argument("--timeout", type=float, default=300.0,
                    help="seconds to wait for the drive in one-shot mode")
    ap.add_argument("--eject-timeout", type=float, default=20.0)
    ap.add_argument("--out", default="build/rp2040-autoflash")
    ap.add_argument("--probe", action="store_true", help="send CR + uptime after the log")
    ap.add_argument("--accept", action="store_true",
                    help="run basic shell acceptance commands after the log")
    ap.add_argument("--replug", action="store_true",
                    help="after acceptance, A/B clk_peri + force a fresh USB replug and dump the IRQ trace")
    ap.add_argument("--sysclk-ab", "--usb-ab", dest="sysclk_ab", action="store_true",
                    help="A/B the E15 workaround (on/off) with a fresh replug + IRQ trace each")
    ap.add_argument("--ctrl-d", action="store_true", help="test soft BOOTSEL entry after the log")
    ap.add_argument("--ctrl-d-secs", type=float, default=8.0,
                    help="seconds to wait for the ROM MSC volume to re-enumerate")
    ap.add_argument("--rounds", type=int, default=0, help="stop after N rounds (0 = unlimited)")
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    queue = list(args.uf2)
    idx = 0
    round_no = 0
    # armed means "a fresh press has happened".  We start armed so an already
    # mounted BOOTSEL drive is flashed immediately.
    armed = True

    while True:
        if not armed:
            # Wait for the volume to disappear before re-arming, otherwise the
            # drive left over from the previous round would trigger a reflash.
            while bootsel_drive():
                time.sleep(0.2)
            armed = True
            print("[watch] drive gone, re-armed for next press", flush=True)

        drv = wait_for_drive(None if args.watch else args.timeout,
                             lambda m: print("[watch] " + m, flush=True))
        if not drv:
            print("ABORT: no BOOTSEL drive after %.0fs" % args.timeout, flush=True)
            return 1

        round_no += 1
        uf2 = queue[idx % len(queue)]
        idx += 1
        stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
        transcript = ["RP2040 autoflash | round %d | %s | image %s" %
                      (round_no, stamp, uf2)]
        ok = run_round(args, uf2, drv, round_no, transcript)
        transcript.append("result: %s" % ("ok" if ok else "FAILED"))
        path = os.path.join(args.out, "%s-r%d.log" % (stamp, round_no))
        with open(path, "w", encoding="utf-8", errors="replace") as f:
            f.write("\n".join(transcript) + "\n")
        print("[round %d] transcript: %s" % (round_no, path), flush=True)

        if not args.watch or (args.rounds and round_no >= args.rounds):
            return 0 if ok else 1
        if ok:
            armed = False           # wait for this volume to disappear
        else:
            armed = True            # retry the same mount (e.g. port race)
            time.sleep(2.0)


if __name__ == "__main__":
    sys.exit(main())
