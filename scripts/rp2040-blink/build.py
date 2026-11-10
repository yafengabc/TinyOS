#!/usr/bin/env python3
"""Build the RP2040 bring-up blinker and wrap it in a UF2.

The image is boot2 (256 bytes at 0x10000000) + the payload linked at
0x10000100.  Boot2 is not generated here: it is copied out of an existing
UF2 that is known to boot on the board, by default the MicroPython image
(this matters, because the TinyOS-built boot2 was never proven on hardware):

    python scripts/rp2040-blink/build.py \
        --boot2-from build/micropython-rpi_pico.uf2 -o build/rp2040-blink.uf2

Copy the result to the RPI-RP2 mass-storage device, then look at GP0 with a
scope.  If it toggles, flash layout, boot2 and the vector table are all
sound and the problem is elsewhere in the OS image.
"""

import argparse
import os
import struct
import subprocess
import sys

UF2_MAGIC0 = 0x0A324655
UF2_MAGIC1 = 0x9E5D5157
UF2_FAMILY_RP2040 = 0xE48BFF56
FLASH_BASE = 0x10000000
PAYLOAD_BASE = 0x10000100

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def boot2_from_uf2(path):
    data = open(path, "rb").read()
    for off in range(0, len(data) - 512 + 1, 512):
        magic0, magic1, _flags, addr, size = struct.unpack_from("<IIIII", data, off)
        if magic0 == UF2_MAGIC0 and magic1 == UF2_MAGIC1 and addr == FLASH_BASE and size == 256:
            return data[off + 32:off + 32 + 256]
    sys.exit("no 256-byte block at 0x10000000 in %s" % path)


def payload_from_elf(path):
    data = open(path, "rb").read()
    if data[:4] != b"\x7fELF":
        sys.exit("%s is not an ELF file" % path)
    phoff = struct.unpack_from("<I", data, 28)[0]
    phentsize = struct.unpack_from("<H", data, 42)[0]
    phnum = struct.unpack_from("<H", data, 44)[0]
    for i in range(phnum):
        p = phoff + i * phentsize
        p_type, p_offset, _vaddr, p_paddr, p_filesz = struct.unpack_from("<IIIII", data, p)
        if p_type == 1 and p_paddr == PAYLOAD_BASE:
            return data[p_offset:p_offset + p_filesz]
    sys.exit("no loadable segment at 0x%08x in %s" % (PAYLOAD_BASE, path))


def make_uf2(raw):
    blocks = (len(raw) + 255) // 256
    out = bytearray()
    for i in range(blocks):
        chunk = raw[i * 256:(i + 1) * 256].ljust(256, b"\0")
        head = struct.pack(
            "<IIIIIIII",
            UF2_MAGIC0, UF2_MAGIC1,
            0x2000,                     # familyID present
            FLASH_BASE + i * 256,
            256, i, blocks,
            UF2_FAMILY_RP2040,
        )
        out += head + chunk + b"\0" * (476 - len(chunk))
        out += struct.pack("<I", 0x0AB16F30)
    return bytes(out)


def run(cmd):
    subprocess.run(cmd, check=True, cwd=HERE)


def main():
    ap = argparse.ArgumentParser()
    default_boot2 = os.path.join(ROOT, "build", "micropython-rpi_pico.uf2")
    ap.add_argument("--boot2-from", default=default_boot2,
                    help="UF2 to lift the 256-byte boot2 from")
    ap.add_argument("-o", "--output", default=os.path.join(ROOT, "build", "rp2040-blink.uf2"))
    args = ap.parse_args()

    cflags = ["--target=armv6m-none-eabi", "-mcpu=cortex-m0plus", "-mthumb", "-O1",
              "-ffreestanding", "-nostdlib", "-fno-builtin"]
    run(["clang"] + cflags + ["-c", "start.S", "-o", "start.o"])
    run(["clang"] + cflags + ["-c", "blink.c", "-o", "blink.o"])
    run(["clang"] + cflags + ["-nostdlib", "-fuse-ld=lld",
                              "-Wl,-T,link.ld", "start.o", "blink.o", "-o", "blink.elf"])

    boot2 = boot2_from_uf2(args.boot2_from)
    payload = payload_from_elf(os.path.join(HERE, "blink.elf"))
    raw = boot2 + payload
    uf2 = make_uf2(raw)

    os.makedirs(os.path.dirname(os.path.abspath(args.output)), exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(uf2)

    sp, reset = struct.unpack_from("<II", raw, 0x100)
    print("payload %d bytes, sp=0x%08x reset=0x%08x" % (len(payload), sp, reset))
    if not reset & 1:
        sys.exit("reset vector is even (0x%08x): the CPU will fault on entry" % reset)
    print("wrote %s (%d bytes)" % (args.output, len(uf2)))


if __name__ == "__main__":
    main()
