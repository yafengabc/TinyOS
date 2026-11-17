# targets/

One file per image.  A target names a board, an ISA and the image knobs; the
board's own facts live in `board/<board>/board.conf`, and the chip's facts in
`internal/build/chip.go`.  That is the three-layer split from
`docs/ARCHITECTURE-NEXT.md` (C1): chip, board, image.

```ini
[target]
name = atk-dnr2350am-smp      # optional; defaults to the file name
board = atk-dnr2350am # board/<board>/board.conf must exist
arch = arm            # must be an ISA of that board's chip
march = rv32imac      # optional; the chip table supplies the default
cpus = 2              # optional, 1 when absent
heap = 60k            # kernel heap, bytes or k/m suffix
max_procs = 8         # optional
kstack = 8k           # optional; per-arch default otherwise
```

Adding an image is a new file here.  Adding a board is a `board/<name>/`
directory with a `board.conf` (and, if it is a variant of another board,
`base = <board>` plus the files it replaces).  Neither needs a Go change.

`go run ./cmd/tos build all` builds every file in this directory (15 images
today, the two chip baselines included).  It ignores `-config` on purpose:
an image's config names the target it was written for.

## Why the files are not generated

They are source, not build output: heap sizes and process slots are decisions
with measurements behind them (see the entries below), and a generator would
have to read them from somewhere anyway.

## Notes worth keeping

- **heap**: the kernel heap holds one decompressed program image at a time and
  littlefs's working set.  Since exec() streams the LZ4 block straight into
  the task window, the peak is much smaller than it used to be; the numbers
  here are still the ones the boards were validated with, and shrinking them
  needs its own measurement, not a guess.
- **rpi-pico / rpi-pico-smp**: `max_procs = 8`, `kstack = 8k` - a proc_t is ~792 B
  and the shell only ever runs a few tasks; the four slots it drops return
  3 KiB of .bss.
- **esp32c3**: `max_procs = 8` and an 80 KiB heap; `board/esp32c3/board.conf`
  supplies `xip_flash_base`, which is what selects `kernel-xip.ld`.
- **rp2350 family**: 60 KiB on ARM (validated size), 32 KiB on the RISC-V
  image.  `atk-dnr2350am-rv32-smp` keeps the same 32 KiB: the second kernel stack comes
  out of .bss, not the heap.
- **rv32nc** exists so the cost of RVC can be measured; it is `rv32im`, no C
  extension, everything else identical to `rv32`.
