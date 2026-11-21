"""Print a program's task-window footprint.

The kernel does not guess: mklfs writes span/stack/heap hints into the image
(PT_NOTE named "TinyOS", type 1 = stack, 2 = heap) and the loader validates
span + stack + heap + guard against the board's grain.  This reads the same
numbers straight out of the ELF so a configuration's budget can be checked
without a board, which is what issue #39 asks for and what elfwin.py was
supposed to be.

    python scripts/elfwin.py build/user/bin/rcc build/user/bin/hugeir
    python scripts/elfwin.py build/user/bin/* --grain 64k --guard 4k

Defaults come from the RP2040 board (grain 4k, guard 4k) because that is the
tightest window in the tree; pass --grain/--guard for another board.  Takes a
raw ELF, which is what the build stages in build/user/bin and build/fs-stage.
"""
import argparse
import os
import struct
import sys

PT_LOAD = 1
PT_NOTE = 4


def kib(n):
    return f"{n / 1024:.1f} KiB"


def parse_size(text):
    t = text.strip().lower()
    mult = 1
    for suffix, m in (("kib", 1024), ("kb", 1024), ("k", 1024),
                      ("mib", 1 << 20), ("mb", 1 << 20), ("m", 1 << 20)):
        if t.endswith(suffix):
            t, mult = t[:-len(suffix)], m
            break
    return int(t) * mult


def hints(path):
    """Return (span, stack, heap) the way mklfs computes them."""
    raw = open(path, "rb").read()
    if raw[:4] != b"\x7fELF":
        raise SystemExit(f"{path}: not a raw ELF (pass build/user/bin/*)")
    if struct.unpack_from("<H", raw, 18)[0] != 40:  # e_machine ARM
        pass  # the layout is the same for the purposes of this tool
    phoff = struct.unpack_from("<I", raw, 28)[0]
    phentsize = struct.unpack_from("<H", raw, 42)[0]
    phnum = struct.unpack_from("<H", raw, 44)[0]

    lo, hi = None, 0
    notes = []
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type, p_offset, p_vaddr, _, p_filesz, p_memsz = struct.unpack_from("<6I", raw, off)
        if p_type == PT_LOAD and p_memsz:
            lo = p_vaddr if lo is None or p_vaddr < lo else lo
            hi = max(hi, p_vaddr + p_memsz)
        elif p_type == PT_NOTE:
            notes.append((p_offset, p_filesz))
    if lo is None or hi <= lo:
        raise SystemExit(f"{path}: no usable PT_LOAD")

    span = (hi - lo + 4095) // 4096 * 4096
    stack = heap = 0
    for p_off, p_filesz in notes:
        blob = raw[p_off:p_off + p_filesz]
        while len(blob) >= 12:
            namesz, descsz, ntype = struct.unpack_from("<3I", blob, 0)
            desc_off = 12 + ((namesz + 3) & ~3)
            nxt = desc_off + ((descsz + 3) & ~3)
            if nxt > len(blob) or nxt <= 12:
                break
            if namesz >= 6 and descsz >= 4 and blob[12:18] == b"TinyOS":
                value = struct.unpack_from("<I", blob, desc_off)[0]
                if ntype == 1 and stack == 0:
                    stack = value
                elif ntype == 2 and heap == 0:
                    heap = value
            blob = blob[nxt:]
    return span, stack, heap


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("elf", nargs="+")
    ap.add_argument("--grain", default="4k", help="board [window] grain (default 4k)")
    ap.add_argument("--guard", default="4k", help="board window guard (default 4k)")
    ap.add_argument("--area", default=None,
                    help="user area PLAT_APP_MAX-PLAT_APP_BASE; when given, "
                         "report whether the window fits (RP2040: 216k)")
    ap.add_argument("--grow", default="0",
                    help="plat_window_grow_reserve() when the image carries no "
                         "stack hint (default 0, i.e. the image has one)")
    args = ap.parse_args()

    grain = parse_size(args.grain)
    guard = parse_size(args.guard)
    grow = parse_size(args.grow)
    area = parse_size(args.area) if args.area else None
    worst = 0
    for path in args.elf:
        span, stack, heap = hints(path)
        raw_size = os.path.getsize(path)
        img = max(span, raw_size)
        need = img + stack + heap + guard + grow
        window = max((need + grain - 1) // grain * grain, grain)
        worst = max(worst, window)
        print(f"{path}\n"
              f"  span {kib(span)}  stack {kib(stack)}  heap {kib(heap)}  "
              f"guard {kib(guard)}\n"
              f"  window {kib(window)}"
              + (f" of {kib(area)} user area -> "
                 f"{'fits' if window <= area else 'TOO BIG'}" if area else ""))
    if len(args.elf) > 1:
        print(f"\nworst window {kib(worst)}")
    return 0 if area is None or worst <= area else 1


if __name__ == "__main__":
    sys.exit(main())
