commit: 6d54b5a
change: irc streams ELF and LZ4TOS output; removes the 36 KiB out buffer and the 12 KiB fixup table
baseline: 4859ec6
target/config: esp32c3-generic / configs/esp32c3-csc3.conf
board/port/image: ESP32-C3 (COM4)
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
    python tools/test.py -> PASS, all 18 native images generated
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
T3: ESP32-C3 COM4 -> PASS
T4: NOT RUN
logs: build/t3-c-irc-stream.log (local)
notes:
- `irc` is now two-pass.  Pass 1 computes text size and every label; pass 2 writes
  machine code directly to the final `.tos`.  The literal-LZ4TOS header and token
  are written first, then the ELF header/phdrs, text, padding and arena.
- Removed: `out[36864]`, `fixes[1024]` (12 KiB), `resolve_fixups()`.
  Kept: the 16 KiB RIR2 input buffer.  Writes are sequential; no littlefs
  temp files are used.
- Window: 159744 -> 73728 bytes (156 -> 72 KiB).  Span: 139264 -> 53248 bytes.
  `/bin/irc`: 40809 -> 18104 bytes.
- On-board check: `csc3 /fold.c /fold.rir` + `irc /fold.rir /bin/sfold` +
  `sfold` prints 14/3; the same path for `str.c` prints the full string output.
- Standalone project commit: `335c084`.
