#!/usr/bin/env python3
"""Generate the 5x7 text font used by the on-board LCD console.

Why a generator instead of a pasted blob: the glyphs are hand-drawn, so they
need to be reviewable.  The art below is the source of truth; the C table is
derived from it and the preview PNG is how a mistake in a glyph gets caught
before it reaches hardware.

    python scripts/gen-font5x7.py            # writes the header + preview
    python scripts/gen-font5x7.py --check    # fails if the header is stale

The preview is rendered the way the panel draws it: 5x7 glyph in a 6x8 cell,
white on black, 40 columns x 16 rows - exactly one 135x240 screen at 1x.
"""

import argparse
import os
import sys

# One entry per printable ASCII character, 0x20..0x7e.  '#' is an ink pixel,
# '.' is background.  Seven rows of five columns, top row first.
GLYPHS = {
    " ": [".....", ".....", ".....", ".....", ".....", ".....", "....."],
    "!": ["..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."],
    '"': [".#.#.", ".#.#.", ".....", ".....", ".....", ".....", "....."],
    "#": [".#.#.", ".#.#.", "#####", ".#.#.", "#####", ".#.#.", ".#.#."],
    "$": ["..#..", ".####", "#.#..", ".###.", "..#.#", "####.", "..#.."],
    "%": ["##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##"],
    "&": [".##..", "#..#.", "#.#..", ".#...", "#.#.#", "#..#.", ".##.#"],
    "'": ["..#..", "..#..", ".....", ".....", ".....", ".....", "....."],
    "(": ["...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."],
    ")": [".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."],
    "*": [".....", "#.#.#", ".###.", "#####", ".###.", "#.#.#", "....."],
    "+": [".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."],
    ",": [".....", ".....", ".....", ".....", "..##.", "..#..", ".#..."],
    "-": [".....", ".....", ".....", "#####", ".....", ".....", "....."],
    ".": [".....", ".....", ".....", ".....", ".....", ".##..", ".##.."],
    "/": ["....#", "....#", "...#.", "..#..", ".#...", "#....", "#...."],
    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    "3": ["#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."],
    "4": ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    "5": ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    "6": ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    "8": [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    "9": [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],
    ":": [".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."],
    ";": [".....", ".##..", ".##..", ".....", ".##..", "..#..", ".#..."],
    "<": ["...#.", "..#..", ".#...", "#....", ".#...", "..#..", "...#."],
    "=": [".....", ".....", "#####", ".....", "#####", ".....", "....."],
    ">": [".#...", "..#..", "...#.", "....#", "...#.", "..#..", ".#..."],
    "?": [".###.", "#...#", "....#", "...#.", "..#..", ".....", "..#.."],
    "@": [".###.", "#...#", "#.###", "#.#.#", "#.###", "#....", ".####"],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "D": ["####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "J": ["..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "N": ["#...#", "##..#", "#.#.#", "#.#.#", "#..##", "#...#", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "Q": [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "W": ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    "[": [".###.", ".#...", ".#...", ".#...", ".#...", ".#...", ".###."],
    "\\": ["#....", "#....", ".#...", "..#..", "...#.", "....#", "....#"],
    "]": [".###.", "...#.", "...#.", "...#.", "...#.", "...#.", ".###."],
    "^": ["..#..", ".#.#.", "#...#", ".....", ".....", ".....", "....."],
    "_": [".....", ".....", ".....", ".....", ".....", ".....", "#####"],
    "`": [".#...", "..#..", ".....", ".....", ".....", ".....", "....."],
    "a": [".....", ".....", ".###.", "....#", ".####", "#...#", ".####"],
    "b": ["#....", "#....", "####.", "#...#", "#...#", "#...#", "####."],
    "c": [".....", ".....", ".###.", "#....", "#....", "#...#", ".###."],
    "d": ["....#", "....#", ".####", "#...#", "#...#", "#...#", ".####"],
    "e": [".....", ".....", ".###.", "#...#", "#####", "#....", ".###."],
    "f": ["..##.", ".#...", "####.", ".#...", ".#...", ".#...", ".#..."],
    "g": [".....", ".....", ".####", "#...#", ".####", "....#", ".###."],
    "h": ["#....", "#....", "####.", "#...#", "#...#", "#...#", "#...#"],
    "i": ["..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."],
    "j": ["...#.", ".....", "..##.", "...#.", "...#.", "#..#.", ".##.."],
    "k": ["#....", "#....", "#..#.", "#.#..", "##...", "#.#..", "#..#."],
    "l": [".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "m": [".....", ".....", "##.#.", "#.#.#", "#.#.#", "#...#", "#...#"],
    "n": [".....", ".....", "####.", "#...#", "#...#", "#...#", "#...#"],
    "o": [".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."],
    "p": [".....", ".....", "####.", "#...#", "####.", "#....", "#...."],
    "q": [".....", ".....", ".####", "#...#", ".####", "....#", "....#"],
    "r": [".....", ".....", "#.##.", "##...", "#....", "#....", "#...."],
    "s": [".....", ".....", ".####", "#....", ".###.", "....#", "####."],
    "t": [".#...", ".#...", "####.", ".#...", ".#...", ".#..#", "..##."],
    "u": [".....", ".....", "#...#", "#...#", "#...#", "#...#", ".####"],
    "v": [".....", ".....", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "w": [".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#."],
    "x": [".....", ".....", "#...#", ".#.#.", "..#..", ".#.#.", "#...#"],
    "y": [".....", ".....", "#...#", "#...#", ".####", "....#", ".###."],
    "z": [".....", ".....", "#####", "...#.", "..#..", ".#...", "#####"],
    "{": ["...#.", "..#..", "..#..", ".#...", "..#..", "..#..", "...#."],
    "|": ["..#..", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "}": [".#...", "..#..", "..#..", "...#.", "..#..", "..#..", ".#..."],
    "~": [".....", ".....", ".#..#", "#.#.#", "#..#.", ".....", "....."],
}

FIRST = 0x20
LAST = 0x7E
GLYPH_W = 5
GLYPH_H = 7
CELL_W = 6
CELL_H = 8


def columns(art):
    """Turn seven rows of art into five column bytes, bit 0 = top row."""
    cols = []
    for x in range(GLYPH_W):
        bits = 0
        for y in range(GLYPH_H):
            if art[y][x] == "#":
                bits |= 1 << y
        cols.append(bits)
    return cols


def build():
    table = []
    for code in range(FIRST, LAST + 1):
        ch = chr(code)
        if ch not in GLYPHS:
            raise SystemExit(f"missing glyph for {code:#04x} {ch!r}")
        art = GLYPHS[ch]
        if len(art) != GLYPH_H or any(len(r) != GLYPH_W for r in art):
            raise SystemExit(f"glyph {ch!r} is not {GLYPH_W}x{GLYPH_H}")
        table.append((ch, columns(art)))
    missing = set(GLYPHS) - {chr(c) for c in range(FIRST, LAST + 1)}
    if missing:
        raise SystemExit(f"glyphs outside the table: {sorted(missing)}")
    return table


def emit_header(table, path):
    lines = []
    lines.append("/* Generated by scripts/gen-font5x7.py - do not edit.")
    lines.append(" *")
    lines.append(" * 5x7 ASCII, one byte per column, bit 0 = top row, drawn in a")
    lines.append(" * 6x8 cell (one column of spacing on the right, one row below).")
    lines.append(" * 40 columns x 16 rows on a 240x135 panel. */")
    lines.append("")
    lines.append("#ifndef TINYOS_FONT5X7_H")
    lines.append("#define TINYOS_FONT5X7_H")
    lines.append("")
    lines.append("#include \"types.h\"")
    lines.append("")
    lines.append(f"#define FONT5X7_FIRST {FIRST}   /* ' ' */")
    lines.append(f"#define FONT5X7_LAST  {LAST}   /* '~' */")
    lines.append(f"#define FONT5X7_W     {GLYPH_W}")
    lines.append(f"#define FONT5X7_H     {GLYPH_H}")
    lines.append("")
    lines.append("static const uint8_t font5x7[FONT5X7_LAST - FONT5X7_FIRST + 1][5] = {")
    for ch, cols in table:
        label = ch if ch not in ('\\', '"') else "\\" + ch
        body = ", ".join(f"0x{c:02x}" for c in cols)
        lines.append(f"    {{ {body} }},   /* '{label}' */")
    lines.append("};")
    lines.append("")
    lines.append("#endif")
    lines.append("")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def emit_preview(table, path):
    """Render one full screen of text the way the panel will."""
    from PIL import Image, ImageDraw

    cols, rows = 40, 16
    scale = 3
    img = Image.new("RGB", (cols * CELL_W * scale, rows * CELL_H * scale), (0, 0, 0))
    px = img.load()

    chars = [c for c, _ in table]
    lines = []
    for r in range(rows):
        lines.append("".join(chars[(r * cols + i) % len(chars)] for i in range(cols)))

    for row, line in enumerate(lines):
        for col, ch in enumerate(line):
            idx = ord(ch) - FIRST
            glyph = table[idx][1]
            for x in range(GLYPH_W):
                bits = glyph[x]
                for y in range(GLYPH_H):
                    if bits & (1 << y):
                        ox = (col * CELL_W + x) * scale
                        oy = (row * CELL_H + y) * scale
                        for dy in range(scale):
                            for dx in range(scale):
                                px[ox + dx, oy + dy] = (255, 255, 255)

    d = ImageDraw.Draw(img)
    d.rectangle([0, 0, img.width - 1, img.height - 1], outline=(40, 40, 40))
    img.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="verify the committed header matches the art")
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    header = os.path.join(root, "board", "rp2350", "font5x7.h")
    preview = os.path.join(root, "build", "font5x7-preview.png")
    ap.add_argument("--header", default=header)
    args = ap.parse_args()

    table = build()

    if args.check:
        import tempfile
        tmp = os.path.join(tempfile.gettempdir(), "font5x7.h")
        emit_header(table, tmp)
        with open(tmp, encoding="utf-8") as f:
            fresh = f.read()
        with open(args.header, encoding="utf-8") as f:
            have = f.read()
        if fresh != have:
            print("font5x7.h is stale: rerun scripts/gen-font5x7.py", file=sys.stderr)
            return 1
        print("font5x7.h is up to date")
        return 0

    emit_header(table, args.header)
    print(f"wrote {args.header} ({len(table)} glyphs)")
    os.makedirs(os.path.dirname(preview), exist_ok=True)
    emit_preview(table, preview)
    print(f"wrote {preview}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
