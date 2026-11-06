"""Poke the board back into BOOTSEL after a hang.

The running firmware watches UART0 for Ctrl-D from its 1 ms tick during the
first 30 s after boot, so all this has to do is keep sending 0x04 while the
user presses RESET.  It stops as soon as the UF2 volume shows up.
"""

import os
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM14"
SECONDS = float(sys.argv[2]) if len(sys.argv) > 2 else 45.0
FLASH = len(sys.argv) > 3 and sys.argv[3] == "--flash"


def bootsel_present():
    for letter in "FGHIJKLMNOPQRSTUVWXYZ":
        root = f"{letter}:\\"
        if os.path.exists(root + "INFO_UF2.TXT"):
            return root
    return None


def main():
    s = serial.Serial(PORT, 115200, timeout=0.2)
    s.setDTR(False)
    s.setRTS(False)
    deadline = time.time() + SECONDS
    while time.time() < deadline:
        drive = bootsel_present()
        if drive:
            s.close()
            print("BOOTSEL drive:", drive, flush=True)
            # Hand over immediately: the UF2 volume can disappear between two
            # processes, and every second spent starting one is a chance to
            # lose the window.
            if FLASH:
                import subprocess
                cmd = [sys.executable, os.path.join(os.path.dirname(__file__), "flash.py"),
                       "-p", "none", "--uart-port", PORT]
                print("flashing:", " ".join(cmd), flush=True)
                return subprocess.call(cmd)
            return 0
        try:
            s.write(b"\x04")
            s.flush()
        except Exception:
            pass
        time.sleep(0.15)
    s.close()
    drive = bootsel_present()
    print("BOOTSEL drive:" if drive else "no BOOTSEL drive", drive or "", flush=True)
    return 1 if not drive else 0


if __name__ == "__main__":
    raise SystemExit(main())
