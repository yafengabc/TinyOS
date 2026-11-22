#!/usr/bin/env python3
"""Generate the shared malformed-input corpus under tests/corpus/.

Each case is a directory with input.bin and expect:

    elf/accept/stack-heap-window-note     accept stack=8192 heap=4096 window=0 span=0x1000
    elf/reject/no-load-segment            reject
    lz4/accept/hello-literals             accept 68656c6c6f

The files are small and committed so both the Go parser and (later) the C
one read the same bytes instead of each test growing its own fixtures.  The
expect format is deliberately coarse: accept/reject plus the parsed numbers.
Never an error string - the three implementations word their errors
differently and pinning the text would freeze the wrong thing.

Run from the repository root:  python tests/corpus/gen.py
"""
import os
import struct
import zlib


ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)))
ELF32_HDR = 52
PHDR = 32


def ehdr(phoff, phnum, machine=40):
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    return ident + struct.pack("<HHIIIIIHHHHHH",
                               2, machine, 1, 0x1000, phoff, 0, 0,
                               ELF32_HDR, PHDR, phnum, 0, 0, 0)


def phdr(ptype, off, vaddr, filesz, memsz, flags=5):
    return struct.pack("<IIIIIIII", ptype, off, vaddr, vaddr,
                       filesz, memsz, flags, 4)


def note(typ, value, namesz=7):
    name = b"TinyOS\0"
    desc = struct.pack("<I", value)
    return (struct.pack("<III", namesz, len(desc), typ) +
            name + b"\0" * ((-len(name)) % 4) + desc + b"\0" * ((-len(desc)) % 4))


def elf(loads, notes):
    """loads: [(vaddr, filesz, memsz)]; notes: [(typ, value)]."""
    phnum = len(loads) + (1 if notes else 0)
    off = ELF32_HDR + phnum * PHDR
    phdrs, body = b"", b""
    for vaddr, filesz, memsz in loads:
        phdrs += phdr(1, off + len(body), vaddr, filesz, memsz)
        body += b"\xc3" * filesz
    if notes:
        blob = b"".join(note(t, v) for t, v in notes)
        phdrs += phdr(4, off + len(body), 0, len(blob), 0, flags=4)
        body += blob
    return ehdr(ELF32_HDR, phnum) + phdrs + body


def write(case, data, expect):
    d = os.path.join(ROOT, case)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "input.bin"), "wb") as f:
        f.write(data)
    with open(os.path.join(d, "expect"), "w", encoding="utf-8", newline="") as f:
        f.write(expect.rstrip() + "\n")


def main():
    # --- ELF: what windowNote() must see -------------------------------
    write("elf/accept/stack-heap-window-note",
          elf([(0x1000, 0x123, 0x123)],
              [(1, 0x2000), (2, 0x1000), (3, 0x4000)]),
          "accept stack=8192 heap=4096 window=16384 span=0x1000")
    # A note that only says TOS_ONE_GRAIN(): window 0, no stack.
    write("elf/accept/one-grain-note",
          elf([(0x2000, 0x40, 0x100)], [(3, 0)]),
          "accept stack=0 heap=0 window=0 span=0x1000")
    # No PT_LOAD at all: there is no span to reserve.
    write("elf/reject/no-load-segment",
          elf([], [(1, 0x1000)]), "reject")
    write("elf/reject/truncated-header",
          elf([(0x1000, 0x10, 0x10)], [])[:40], "reject")
    write("elf/reject/phoff-beyond-file",
          ehdr(0x1000, 1) + b"\0" * 64, "reject")
    # A namesz that would wrap the padded length: the parser must stop
    # reading notes without panicking, and the span still counts.
    bad = bytearray(elf([(0x1000, 0x10, 0x10)], [(1, 0x2000)]))
    at = bad.find(b"TinyOS") - 12
    struct.pack_into("<I", bad, at, 0xFFFF_FFF9)
    write("elf/accept/note-namesz-wrap", bytes(bad),
          "accept stack=0 heap=0 window=0 span=0x1000")
    # Two segments 4 GiB apart: the span cannot be expressed in uint32.
    write("elf/reject/span-overflow",
          elf([(0x1000, 0x10, 0x10), (0xFFFF_FFF0, 0x10, 0x20)], []),
          "reject")

    # --- LZ4 blocks: what lz4DecompressBlock() must see -----------------
    # token = literal length << 4, then the literals.
    write("lz4/accept/hello-literals", bytes([0x50]) + b"hello",
          "accept " + b"hello".hex())
    write("lz4/accept/empty-input-empty-output", b"", "accept ")
    write("lz4/reject/truncated-literals", bytes([0x50]) + b"hel", "reject")
    # Zero literals then a 4-byte match with offset 8 and nothing produced.
    write("lz4/reject/match-before-output", bytes([0x00]) + struct.pack("<H", 8),
          "reject")
    # A literal length that needs continuation bytes the block does not have.
    write("lz4/reject/length-runs-off-end", bytes([0xF0]) + b"\xff" * 2, "reject")

    # A manifest of case names, so a test can prove none were skipped.
    cases = []
    for dirpath, _, files in os.walk(ROOT):
        if "input.bin" in files:
            cases.append(os.path.relpath(dirpath, ROOT).replace(os.sep, "/"))
    cases.sort()
    with open(os.path.join(ROOT, "cases.txt"), "w", encoding="utf-8", newline="") as f:
        f.write("\n".join(cases) + "\n")
    # The CRC pins the bytes: regenerating with a different Python or a
    # different generator shows up as a diff even for the binary files.
    crc = 0
    for c in cases:
        for name in ("input.bin", "expect"):
            with open(os.path.join(ROOT, c, name), "rb") as f:
                crc = zlib.crc32(f.read(), crc)
    print("wrote %d cases, crc32 %08x" % (len(cases), crc & 0xFFFFFFFF))


if __name__ == "__main__":
    main()
