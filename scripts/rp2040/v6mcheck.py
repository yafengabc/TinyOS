"""Fail if a .tos contains anything but ARMv6-M code.

Used by the RP2040 flow: an M0+ faults on the first 32-bit Thumb-2
instruction, and it is easy to stage an image built for the wrong core (for
example after a `--arch thumb2` run left its files in files.local/).

    python scripts/rp2040/v6mcheck.py files.local/bin/t1-*

Only the code before the rodata marker ("bounds\\n") is checked: rcc puts the
string pool and the float tables inside the same text segment.
"""
import glob
import struct
import sys

TEXT_OFF = 0x100


def payload(path):
    d = open(path, "rb").read()
    if d[:6] != b"LZ4TOS":
        raise SystemExit(f"{path}: not a LZ4TOS image")
    out_len = struct.unpack_from("<I", d, 8)[0]
    p = 28
    n = d[p] >> 4
    p += 1
    if n == 15:
        while True:
            b = d[p]
            p += 1
            n += b
            if b != 255:
                break
    elf = d[p:p + n]
    if len(elf) != out_len:
        raise SystemExit(f"{path}: short payload")
    phoff = struct.unpack_from("<I", elf, 28)[0]
    phentsize = struct.unpack_from("<H", elf, 42)[0]
    phnum = struct.unpack_from("<H", elf, 44)[0]
    for i in range(phnum):
        off = phoff + i * phentsize
        if struct.unpack_from("<I", elf, off)[0] == 1 and elf[off + 24] == 5:
            o, sz = struct.unpack_from("<I", elf, off + 4)[0], struct.unpack_from("<I", elf, off + 16)[0]
            return elf[o:o + sz]
    raise SystemExit(f"{path}: no executable segment")


def bad_instructions(code):
    cut = code.find(b"bounds\n")
    if cut > 0:
        code = code[:cut]
    i, bad = 0, []
    while i + 1 < len(code):
        w = code[i] | (code[i + 1] << 8)
        if (w & 0xF800) >= 0xE800:              # 32-bit encoding space
            if i + 3 < len(code):
                w2 = code[i + 2] | (code[i + 3] << 8)
                is_bl = (w & 0xF800) == 0xF000 and (w2 & 0xD000) == 0xD000
                if not is_bl:
                    bad.append((i, w, w2))
            i += 4
            continue
        i += 2
    return bad


def main(paths):
    expanded = []
    for p in paths:
        hits = glob.glob(p)
        expanded.extend(hits if hits else [p])
    paths = expanded
    total = 0
    for path in paths:
        code = payload(path)
        bad = bad_instructions(code)
        if bad:
            total += len(bad)
            first = ", ".join(f"0x{off:x}:{w:04x}{w2:04x}" for off, w, w2 in bad[:3])
            print(f"{path}: {len(bad)} non-v6-M instruction(s): {first}")
    if total:
        print(f"ARMv6-M CHECK FAILED: {total} instruction(s)")
        return 1
    print(f"ARMv6-M CHECK OK ({len(paths)} image(s))")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: v6mcheck.py <file.tos> ...")
    sys.exit(main(sys.argv[1:]))
