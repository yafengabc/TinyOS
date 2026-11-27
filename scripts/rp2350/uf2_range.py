"""Report the flash range a UF2 actually programs, for RP2040 and RP2350.

Blocks are 512 bytes: magic0/1/2, flags, targetAddr, payloadSize, blockNo,
numBlocks, family, then up to 476 bytes of payload.

RP2350 images start with the E10 workaround block: 256 bytes of 0xef at
0x10ffff00 tagged RP2_IGNORE_BLOCK (0x9957e304), which tells the ROM to skip
that sector.  It is metadata, not a write, so it must not count towards the
address range - counting it makes every RP2350 image look like it writes
16 MiB.  Extension blocks carry UF2_FLAG_EXTENSION (0x8000) and put their tag
in the last word of the payload.
"""
import struct
import sys

MAGIC0, MAGIC1, MAGIC2 = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FLAG_EXTENSION = 0x8000
RP2_IGNORE_BLOCK = 0x9957E304
FLASH_BASE = 0x10000000
# rp2350AbsBlock(): the E10 workaround lives at this address, 256 bytes of
# 0xef, with the ignore tag at b[288] - just past the payload, not inside it.
RP2350_ABS_ADDR = 0x10FFFF00
RP2350_ABS_FAMILY = 0xE48BFF57


def blocks(data):
    for off in range(0, len(data) - 511, 512):
        b = data[off:off + 512]
        m0, m1, flags, addr, size, no, num, fam = struct.unpack("<8I", b[:32])
        if m0 != MAGIC0 or m1 != MAGIC1:
            continue
        if struct.unpack("<I", b[508:512])[0] != MAGIC2:
            continue
        tag = struct.unpack("<I", b[32 + size - 4:32 + size])[0] if size >= 4 else 0
        yield flags, addr, size, fam, tag


def scan(path):
    data = open(path, "rb").read()
    written = []
    ignored = []
    for flags, addr, size, fam, tag in blocks(data):
        e10 = (addr == RP2350_ABS_ADDR and size == 256
               and (fam == RP2350_ABS_FAMILY or tag == RP2_IGNORE_BLOCK))
        if e10 or (flags & FLAG_EXTENSION and tag == RP2_IGNORE_BLOCK):
            ignored.append((addr, size))
            continue
        if size:
            written.append((addr, size, fam))

    written.sort()
    segments = []
    for addr, size, fam in written:
        if segments and addr == segments[-1][1] and fam == segments[-1][2]:
            segments[-1][1] = addr + size
        else:
            segments.append([addr, addr + size, fam])

    total = sum(s for _, s, _ in written)
    top = max((e for _, e, _ in segments), default=FLASH_BASE)
    return segments, total, ignored, top


def main():
    for path in sys.argv[1:]:
        segs, total, ignored, top = scan(path)
        print(path)
        for lo, hi, fam in segs:
            print(f"  writes  0x{lo:08x} .. 0x{hi:08x}   {hi - lo:>9} B   family 0x{fam:08x}")
        for addr, size in ignored:
            print(f"  skips   0x{addr:08x} .. 0x{addr + size:08x}   {size:>9} B   "
                  f"RP2_IGNORE_BLOCK (E10 workaround, ROM does not program it)")
        used = top - FLASH_BASE
        for cap, label in ((4 << 20, "4 MB"), (8 << 20, "8 MB")):
            ok = "fits" if used <= cap else "DOES NOT FIT"
            print(f"  {label:<5} used {used:>9} B of {cap:>9} B -> {ok}, "
                  f"headroom {cap - used:>9} B")
        print(f"  payload {total} B")
        print()


if __name__ == "__main__":
    main()
