#!/usr/bin/env python3
"""Flash TinyOS to an RP2350 over the BOOTSEL UF2 drive, then watch the console.

The RP2350's mask ROM is the bootloader: hold BOOTSEL while plugging the USB
data cable (or press BOOTSEL + RESET) and it mounts a small FAT volume named
"RP2350" (older ROMs call it "RPI-RP2").  Dropping build/rp2350.uf2 onto that
volume writes the flash and reboots.

TinyOS exposes TWO consoles at once: its own USB CDC-ACM COM port (2e8a:000a,
no extra hardware -- just the board's USB cable) and UART0 on GPIO0/1 at
115200 8N1 for a USB-TTL adapter.  --port auto (the default) finds the CDC
port by VID/PID after the reboot; pass -p COM14 to watch the UART instead.

There is no UART flashing protocol.  If the running firmware is pico-sdk
based (2e8a:0009 with a "Reset" interface), --boot-1200 COM15 sends the
standard 1200-baud touch that reboots straight into BOOTSEL, no button press.

Usage:
    python scripts/rp2350/flash.py --smoke              # auto console, wait for BOOTSEL
    python scripts/rp2350/flash.py --boot-1200 COM15 --smoke
    python scripts/rp2350/flash.py --no-flash --smoke   # just watch the console
    python scripts/rp2350/flash.py --boot-via COM9      # MicroPython REPL -> bootloader
    python scripts/rp2350/flash.py --list               # show serial ports / drives

Never copies to a fixed disk: a drive is only used when GetDriveType says
removable AND it carries INFO_UF2.TXT with an RP2/RP2350 board string.
"""

import argparse
import ctypes
import os
import re
import shutil
import string
import sys
import time

import serial

MARK = b"tos> "
DEFAULT_UF2 = os.path.join("build", "rp2350.uf2")
TINYOS_USB_VID_PID = "VID_2E8A&PID_000A"   # board/rp2350/usb_cdc.c


def serial_ports():
    """Read the Windows SERIALCOMM map without a PowerShell call."""
    import winreg

    out = []
    try:
        key = winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DEVICEMAP\SERIALCOMM"
        )
        i = 0
        while True:
            try:
                name, value, _ = winreg.EnumValue(key, i)
                out.append((name, value))
                i += 1
            except OSError:
                break
    except OSError:
        pass
    return out


def removable_drives():
    get_drive_type = ctypes.windll.kernel32.GetDriveTypeW
    get_drive_type.argtypes = [ctypes.c_wchar_p]
    for letter in string.ascii_uppercase:
        root = letter + ":\\"
        if get_drive_type(root) == 2:  # DRIVE_REMOVABLE
            yield root


def bootsel_drive():
    for root in removable_drives():
        info = os.path.join(root, "INFO_UF2.TXT")
        try:
            with open(info, "r", encoding="utf-8", errors="replace") as f:
                text = f.read(2048)
        except OSError:
            continue
        if "RP2350" in text.upper() or "RP2" in text.upper() or "RPI-RP2" in text.upper():
            return root, text.strip().replace("\n", " | ")
    return None, None


def wait_drive(timeout, tick=None):
    print(f"waiting for the BOOTSEL drive (hold BOOTSEL and replug USB) ... {timeout}s")
    end = time.time() + timeout
    while time.time() < end:
        if tick:
            tick()
        root, info = bootsel_drive()
        if root:
            print(f"  found {root}  {info}")
            return root
        time.sleep(0.4)
    return None


def cdc_port():
    """Find TinyOS's own USB CDC COM port by VID/PID, not by a fixed number.

    Windows assigns a new COM number whenever a USB device with a different
    VID/PID enumerates, so a hard-coded COM15 would break after the first
    flash.  Only ports that are currently in SERIALCOMM are returned.
    """
    import winreg

    online = {v for _, v in serial_ports()}
    base = r"SYSTEM\CurrentControlSet\Enum\USB"
    try:
        root = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base)
    except OSError:
        return None
    i = 0
    while True:
        try:
            dev = winreg.EnumKey(root, i)
            i += 1
        except OSError:
            break
        if not dev.upper().startswith(TINYOS_USB_VID_PID):
            continue
        try:
            inst_root = winreg.OpenKey(root, dev)
        except OSError:
            continue
        j = 0
        while True:
            try:
                inst = winreg.EnumKey(inst_root, j)
                j += 1
            except OSError:
                break
            try:
                node = winreg.OpenKey(inst_root, inst)
            except OSError:
                continue
            try:
                name = winreg.QueryValueEx(node, "FriendlyName")[0]
                m = re.search(r"\(COM(\d+)\)", name)
                if m and ("COM" + m.group(1)) in online:
                    return "COM" + m.group(1)
            except OSError:
                pass
            try:
                params = winreg.OpenKey(node, "Device Parameters")
                port = winreg.QueryValueEx(params, "PortName")[0]
                if port in online:
                    return port
            except OSError:
                pass
    return None


def wait_cdc_port(before, timeout, tick=None):
    """Wait for the freshly enumerated CDC port, with a new-port fallback.

    The VID/PID match wins: an old firmware port can reappear transiently
    during the reboot, and that must not be mistaken for the new shell.  Only
    if nothing ever matches do we fall back to whichever port is brand new.
    """
    end = time.time() + timeout
    newest = []
    while time.time() < end:
        if tick:
            tick()
        port = cdc_port()
        if port:
            return port
        newest = sorted({v for _, v in serial_ports()} - before)
        time.sleep(0.2)
    return newest[0] if newest else None


def touch_1200(port):
    """pico-sdk firmware reboots into BOOTSEL when the CDC baud goes to 1200.

    PICO_USB_RESET_MAGIC_BAUD_RATE in the SDK is 1200; the port disappearing
    while we touch it is the expected success path, not an error.
    """
    print(f"  1200-baud touch on {port} (reset interface -> BOOTSEL)")
    try:
        s = serial.Serial(port, 1200, timeout=0.1)
        s.setDTR(False)
        time.sleep(0.15)
        s.close()
    except serial.SerialException as e:
        print(f"  {port} vanished during the touch: {e} (it rebooted)")
    return True


def flash(uf2, root, tick=None):
    """Copy the UF2 in unbuffered 512-byte writes.

    shutil.copyfileobj() with a 1 MiB buffer races the mask ROM: it consumes
    each UF2 block as it lands and can tear the FAT volume down while Windows
    still has a large buffered write in flight, which surfaced as
    FileNotFoundError on the last chunk of a 2.7 MB image.  One write per UF2
    block matches what the ROM expects, keeps the console tailer polled, and
    turns "the drive vanished" into a diagnosable event instead of a crash.
    os.fsync() is deliberately absent: BOOTSEL is a synthetic FAT volume and
    syncing it returns EBADF on some hosts.
    """
    with open(uf2, "rb") as src:
        data = src.read()
    size = len(data)
    dst = os.path.join(root, os.path.basename(uf2))
    print(f"  copying {uf2} ({size:,} bytes) -> {dst}")

    wrote = 0
    vanished = False
    out = None
    try:
        out = open(dst, "wb", buffering=0)
        for off in range(0, size, 512):
            wrote += out.write(data[off:off + 512])
            if tick and (off // 512) % 64 == 0:
                tick()
    except OSError as e:
        vanished = True
        print(f"  drive vanished during copy: {e}")
    finally:
        if out is not None:
            try:
                out.close()
            except OSError:
                pass
    print(f"  wrote {wrote:,}/{size:,} bytes")
    if wrote < size:
        print("  WARNING: image may be incomplete - reflash before trusting the log"
              if vanished else "  WARNING: short copy")
    print("  copy done, waiting for the board to reboot ...")
    end = time.time() + 60
    while time.time() < end:
        if tick:
            tick()
        if not os.path.exists(os.path.join(root, "INFO_UF2.TXT")):
            print("  drive disappeared: firmware accepted, rebooting")
            return True
        time.sleep(0.4)
    print("  warning: drive still present after 60s (check INFO_UF2.TXT for an error)")
    return False


def boot_via_ctrl_d(port):
    """A running TinyOS watches UART0 for Ctrl-D and calls the ROM reboot API.

    Works from a live shell AND from plat_shutdown()'s polling loop, so a
    crashed kernel is still reflashable with no physical BOOT+RST press.
    """
    print(f"  sending Ctrl-D on {port} -> ROM BOOTSEL")
    try:
        s = serial.Serial(port, 115200, timeout=0.1)
        s.setDTR(False)
        s.setRTS(False)
        s.reset_input_buffer()
        s.write(b"\x04")
        s.flush()
        time.sleep(0.3)
        s.close()
        return True
    except serial.SerialException as e:
        print(f"  could not send Ctrl-D on {port}: {e}")
        return False


def reboot_via_micropython(port):
    """Ask a running MicroPython REPL to reboot into the ROM bootloader."""
    print(f"  asking MicroPython on {port} for machine.bootloader()")
    try:
        s = serial.Serial(port, 115200, timeout=0.2)
        # pyserial asserts DTR/RTS on open.  On boards that strap those to
        # RUN/BOOTSEL that would reset the chip before the REPL ever sees
        # the command.
        s.setDTR(False)
        s.setRTS(False)
        time.sleep(0.1)
        s.write(b"\x03")                       # Ctrl-C: leave a running script
        time.sleep(0.2)
        s.write(b"\r\nimport machine; machine.bootloader()\r\n")
        s.flush()
        time.sleep(0.5)
        s.close()
        return True
    except serial.SerialException as e:
        print(f"  could not use {port}: {e}")
        print("  (close the serial monitor holding that port, or press BOOTSEL by hand)")
        return False


class UartTailer:
    """Keep UART0 open across the whole flash/reboot window.

    The boot banner is printed within a few milliseconds of the ROM jumping
    into the kernel.  Opening COM14 only *after* the reboot means that line
    is already gone.  A tailer opens the adapter before the UF2 is copied and
    is polled from every wait loop, so the earliest [A]/[B]/[C]/[D] markers
    survive even if Windows never sees the USB CDC port.
    """

    def __init__(self, port, baud):
        self.port = port
        self.baud = baud
        self.s = None
        self.buf = bytearray()
        self.last_err = None

    def tick(self):
        if self.s is None:
            try:
                self.s = serial.Serial(self.port, self.baud, timeout=0.02)
                # pyserial asserts DTR/RTS on open (both are active-low on a
                # CH343).  Boards that strap those lines to RUN/BOOTSEL would
                # otherwise sit in reset for the entire flash + boot window.
                # De-assert both: we only need RX, not the modem control lines.
                self.s.setDTR(False)
                self.s.setRTS(False)
                self.s.reset_input_buffer()
                print(f"  UART0 tail opened on {self.port} (DTR/RTS de-asserted)")
            except Exception as e:               # port absent or held by a monitor
                self.last_err = e
                return
        try:
            while True:
                data = self.s.read(4096)
                if not data:
                    break
                self.buf.extend(data)
                if len(self.buf) > 1 << 20:
                    del self.buf[:len(self.buf) - (1 << 20)]
        except Exception as e:
            self.last_err = e
            try:
                self.s.close()
            except Exception:
                pass
            self.s = None

    def log(self):
        self.tick()
        return bytes(self.buf)


class Console:
    def __init__(self, port, baud, ser=None):
        self.s = ser if ser is not None else serial.Serial(port, baud, timeout=0.05)
        # Some USB-TTL adapters wire RTS to RUN: opening the port resets the
        # board, which is exactly when we want to start reading.
        self.s.reset_input_buffer()

    def read(self, seconds):
        end = time.time() + seconds
        buf = bytearray()
        while time.time() < end:
            data = self.s.read(4096)
            if data:
                buf.extend(data)
        return bytes(buf)

    def wait_prompt(self, timeout):
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            data = self.s.read(4096)
            if data:
                buf.extend(data)
                if MARK in bytes(buf):
                    return bytes(buf), True
        return bytes(buf), False

    def cmd(self, line, timeout=10.0):
        self.s.write(line.encode() + b"\r\n")
        self.s.flush()
        out = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            data = self.s.read(4096)
            if data:
                out.extend(data)
                if MARK in bytes(out):
                    break
        return bytes(out), MARK in bytes(out)


def smoke(con):
    checks = [
        ("echo hello-rp2350", b"hello-rp2350"),
        ("ls", b"bin"),
        ("free", b""),
        ("uptime", b""),
        ("cat /proc/uptime", b""),
        ("gpio", b""),
        ("reboot", b""),
    ]
    ok = 0
    for line, want in checks:
        out, prompted = con.cmd(line)
        text = out.decode("utf-8", "replace")
        good = want in out and (prompted or line == "reboot")
        print(f"  [{'ok' if good else 'FAIL'}] {line}")
        for ln in text.splitlines():
            if ln.strip():
                print("        " + ln.rstrip())
        ok += 1 if good else 0
    print(f"  {ok}/{len(checks)} checks passed")
    return ok == len(checks)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-p", "--port", default="auto",
                    help="console: auto = find the TinyOS USB CDC port (default), "
                         "or e.g. COM14 for the UART0 adapter")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--uf2", default=DEFAULT_UF2)
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="seconds to wait for the BOOTSEL drive")
    ap.add_argument("--no-flash", action="store_true", help="only watch the console")
    ap.add_argument("--smoke", action="store_true", help="run shell checks after boot")
    ap.add_argument("--boot-via", metavar="PORT",
                    help="send machine.bootloader() to a MicroPython REPL first")
    ap.add_argument("--boot-uart", metavar="PORT",
                    help="send Ctrl-D to a running/crashed TinyOS on UART0 -> "
                         "ROM BOOTSEL, no button press")
    ap.add_argument("--boot-1200", metavar="PORT",
                    help="1200-baud touch on a pico-sdk CDC port -> BOOTSEL, no button")
    ap.add_argument("--uart-port", metavar="PORT",
                    help="if the USB CDC port never appears, fall back to UART0 "
                         "on this port so the boot log is still captured")
    ap.add_argument("--no-uart-tail", action="store_true",
                    help="do not open the UART adapter before flashing at all "
                         "(use when the adapter's DTR/RTS straps to RUN)")
    ap.add_argument("--list", action="store_true", help="show serial ports and removable drives")
    args = ap.parse_args()

    if args.list:
        print("serial ports:")
        for name, value in serial_ports():
            print(f"  {name} -> {value}")
        print(f"TinyOS USB CDC: {cdc_port() or '(not present)'}")
        print("removable drives:")
        for root in removable_drives():
            info = os.path.join(root, "INFO_UF2.TXT")
            print(f"  {root}  {'BOOTSEL UF2' if os.path.exists(info) else ''}")
        return 0

    before_ports = {v for _, v in serial_ports()}

    # Open the UART adapter BEFORE flashing.  The boot banner is a one-shot
    # stream that the kernel emits within milliseconds of the ROM jumping;
    # opening COM14 only after the reboot loses it forever.
    tail = (UartTailer(args.uart_port, args.baud)
            if args.uart_port and not args.no_uart_tail else None)

    def tick():
        if tail:
            tail.tick()

    if tail:
        tick()
        if tail.s is None:
            print(f"  UART0 {args.uart_port} not open yet "
                  f"({tail.last_err}); will keep retrying")

    if args.boot_via:
        reboot_via_micropython(args.boot_via)
    if args.boot_uart:
        # Reuse the tailer's handle when it is the same port: opening COM14
        # a second time would fail with "access denied" and lose the whole
        # boot log that the tailer has been collecting since before the flash.
        if tail and tail.s and args.boot_uart == args.uart_port:
            print(f"  sending Ctrl-D on {args.boot_uart} -> ROM BOOTSEL")
            tail.s.write(b"\x04")
            tail.s.flush()
        else:
            boot_via_ctrl_d(args.boot_uart)
        time.sleep(0.4)
        tick()
    if args.boot_1200:
        touch_1200(args.boot_1200)

    if not args.no_flash:
        if not os.path.isfile(args.uf2):
            print(f"missing {args.uf2}; run: go run ./cmd/tos build -target rp2350",
                  file=sys.stderr)
            return 2
        root = wait_drive(args.timeout, tick)
        if not root:
            print("no BOOTSEL drive appeared. Check that the USB cable carries data,\n"
                  "hold BOOTSEL while plugging it in, then run this again.",
                  file=sys.stderr)
            return 1
        flash(args.uf2, root, tick)

    port = args.port
    if port == "auto":
        port = wait_cdc_port(before_ports, 15.0, tick)
        if not port and args.uart_port:
            print(f"no USB CDC port appeared; falling back to UART0 {args.uart_port}")
            print("LED: blinking = kernel alive, solid = died early, off = ROM never jumped")
            port = args.uart_port

    # Always show what UART0 saw, even when the CDC port won and even if we
    # are about to fail: [A]..[D] locate a hang with no debugger.
    if tail:
        early = tail.log()
        print("---- UART0 early log (captured across the reboot) ----")
        if early:
            for line in early.decode("utf-8", "replace").splitlines():
                print("  " + line.rstrip())
        else:
            print("  (nothing; check GPIO0 TX / GPIO1 RX / GND and the adapter)")
        print("-----------------------------------------------------")

    if not port:
        print("no TinyOS USB CDC port appeared. Is the board's USB cable still\n"
              "plugged in? Try --list, or -p COMxx for the UART0 adapter.",
              file=sys.stderr)
        return 1

    print(f"opening {port} at {args.baud} ...")
    try:
        ser = tail.s if (tail and port == args.uart_port) else None
        con = Console(port, args.baud, ser=ser)
        if ser is None and port == args.uart_port:
            # A freshly opened CH343 must not hold the board in reset.
            con.s.setDTR(False)
            con.s.setRTS(False)
    except serial.SerialException as e:
        print(f"cannot open {port}: {e}", file=sys.stderr)
        print("close any serial monitor holding that port and retry.", file=sys.stderr)
        return 1

    if args.smoke:
        boot, prompted = con.wait_prompt(20.0)
        if not prompted:
            # We may have opened the port after the one-shot boot prompt was
            # printed.  An empty line makes the shell print a fresh one.
            con.s.write(b"\r\n")
            con.s.flush()
            more, prompted = con.wait_prompt(5.0)
            boot += more
    else:
        boot = con.read(6.0)
        prompted = MARK in boot
    text = boot.decode("utf-8", "replace")
    print("---- boot log ----")
    for line in text.splitlines():
        print(line.rstrip())
    print("------------------")
    if not prompted:
        print("no shell prompt seen. Check that the UF2 was accepted, that the\n"
              "USB cable carries data, and (for UART0) GPIO0 TX / GPIO1 RX / GND.",
              file=sys.stderr)
        return 1
    print("shell is up.")
    if args.smoke:
        return 0 if smoke(con) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
