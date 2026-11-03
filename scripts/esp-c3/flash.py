#!/usr/bin/env python3
"""Flash the esp32c3 target and wait for its prompt.

Two images go to flash: the kernel at offset 0 and the littlefs image at
0x80000 (PLAT_LFS_LBA * 512; the first 512 KiB of flash is reserved for the
kernel).  Writing the second one replaces whatever the filesystem held, so
pass --keep-fs to update only the kernel.

The FS image is only 300 KiB while the partition is 3.5 MiB: on first mount
the kernel takes the size from the image superblock and lfs_fs_grow()s into
the real partition.  littlefs 2.9 finds free blocks by tree reachability and
erases on first use, so whatever an older 3.5 MiB image left in the tail is
reclaimed block by block - no erase_region pass is needed.

    scripts/esp-c3/flash.py                  # build is assumed done
    scripts/esp-c3/flash.py --build          # tos build -target esp32c3 first
    scripts/esp-c3/flash.py --keep-fs -p COM4

The board's CH343 has DTR/RTS wired to the auto-download circuit, so esptool
enters download mode by itself.  After writing we deliberately do NOT let
esptool reset the chip (--after no-reset) while nothing is listening: instead
we open the port, pulse EN ourselves, and read what the board prints.  That is
the same trick run.py uses for the probes - a hard reset fires before the port
is reopened and the banner is lost.
"""

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parents[2]
KERNEL = ROOT / "build" / "esp32c3.bin"
FS = ROOT / "build" / "ramdisk-esp32c3.bin"
FS_OFFSET = 0x80000


def esptool(port, *args):
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c3", "-p", port,
           "-b", "460800", *args]
    print("+ " + " ".join(cmd[-6:]), flush=True)
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="COM4")
    ap.add_argument("--build", action="store_true", help="run 'tos build -target esp32c3' first")
    ap.add_argument("--keep-fs", action="store_true", help="do not rewrite the littlefs image")
    ap.add_argument("--wait", type=float, default=20.0, help="seconds to wait for the prompt")
    args = ap.parse_args()

    if args.build:
        subprocess.run([str(ROOT / "tos.exe"), "build", "-target", "esp32c3"],
                       cwd=ROOT, check=True)

    for p in (KERNEL,) + (() if args.keep_fs else (FS,)):
        if not p.exists():
            sys.exit(f"missing {p}: build the esp32c3 target first")

    esptool(args.port, "--after", "no-reset", "write-flash", "0x0", str(KERNEL))
    if not args.keep_fs:
        esptool(args.port, "--after", "no-reset", "write-flash", hex(FS_OFFSET), str(FS))

    port = serial.Serial(args.port, 115200, timeout=0.05)
    port.dtr = False          # IO9 high: boot from flash, not download mode
    port.rts = True           # EN low: hold in reset
    time.sleep(0.15)
    port.reset_input_buffer()
    port.rts = False          # release

    seen = bytearray()
    deadline = time.time() + args.wait
    while time.time() < deadline:
        chunk = port.read(256)
        if chunk:
            seen += chunk
            sys.stdout.write(chunk.decode("utf-8", "backslashreplace"))
            sys.stdout.flush()
            if b"tos> " in seen:
                break

    port.write(b"uptime\r\n")
    time.sleep(0.6)
    tail = port.read(4096)
    sys.stdout.write(tail.decode("utf-8", "backslashreplace"))
    port.close()

    if b"tos> " in seen:
        print(f"\n[ok] prompt after {len(seen)} bytes", flush=True)
        return 0
    print(f"\n[fail] no prompt in {args.wait:.0f}s, got {len(seen)} byte(s)", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(main())
