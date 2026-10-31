#!/usr/bin/env python3
"""Decode the pmp.S report out of a captured serial stream.

A word of all zeroes is ambiguous on its own -- an entry can be off or absent --
so the entry count comes from umode.S, which writes a marker into pmpaddr15 and
reads it back.  This script only reports what the CSRs said.
"""

import re
import sys


def napot(addr):
    """NAPOT pmpaddr -> (base, size)."""
    n = 0
    while n < 32 and (addr >> n) & 1:
        n += 1
    return (addr & ~((1 << n) - 1)) << 2, 1 << (n + 3)


def main(path):
    text = open(path, "rb").read().decode("latin-1")

    # Every watchdog reset starts a fresh run; take the last complete one.
    report = text.rsplit("PMPCFG0..3 A0..15", 1)
    if len(report) < 2:
        print(f"{path}: no PMP report -- the probe never reached its banner")
        return 1

    words = re.findall(r"^([0-9a-f]{8})\r?$", report[1], re.M)
    if len(words) < 20:
        print(f"{path}: only {len(words)} of 20 words came back")
        return 1

    vals = [int(w, 16) for w in words[:20]]
    cfgs, addrs = vals[:4], vals[4:]

    print("pmpcfg0..3 =", " ".join(f"{v:08x}" for v in cfgs))
    print()
    print("entry  cfg   A      L  rwx  range")
    for i in range(16):
        cfg = (cfgs[i // 4] >> ((i % 4) * 8)) & 0xFF
        a = cfg >> 3 & 3
        locked = cfg >> 7 & 1
        rwx = "".join(c for b, c in ((4, "x"), (2, "w"), (1, "r")) if cfg & b) or "---"
        if a == 0:
            rng = ""
        elif a == 1:
            lo = (addrs[i - 1] << 2) if i else 0
            rng = f"0x{lo:08x}..0x{addrs[i] << 2:08x}"
        elif a == 3:
            base, size = napot(addrs[i])
            rng = f"0x{base:08x}+0x{size:x}"
        else:
            rng = f"0x{addrs[i] << 2:08x}+4"
        name = {0: "off", 1: "TOR", 2: "NA4", 3: "NAPOT"}[a]
        print(f"  {i:2d}  0x{cfg:02x}  {name:5s}  {locked}  {rwx}  {rng}")

    active = [i for i in range(16) if ((cfgs[i // 4] >> ((i % 4) * 8)) & 0x1F)]
    locked = [i for i in range(16) if ((cfgs[i // 4] >> ((i % 4) * 8)) & 0x80)]
    print()
    print("active:", active or "none")
    print("locked:", locked or "none")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "build/exp-c3/last.raw"))
