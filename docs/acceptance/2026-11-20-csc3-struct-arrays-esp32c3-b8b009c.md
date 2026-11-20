commit: b8b009c
change: csc3 struct arrays (`ps[i].x`), struct assignment (`q = a`), and `printf("%.Nf")`; `rirvm` bounds op and fixed-precision float output
baseline: 3a0be0a
target/config: esp32c3-generic / configs/esp32c3-csc3.conf
board/port/image: ESP32-C3 (COM4); build/tinyos-esp32c3-generic-full.bin
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
T3: ESP32-C3 COM4 -> PASS
T4: NOT RUN
logs: build/t3-csc3-fullstruct-b8b009c.log, build/t3-csc3-structbounds-b8b009c.log (local only)
notes:
- Source was transferred with `tools/tosput/tosput-cli.exe COM4 <file> /full.c` (495 B, crc32 96fe5f41), not msh echo: msh has MAX_ARGS 8 / LINE_MAX 256 and `>` leaves stale bytes when a shorter file reuses a name.
- Full board test `/full.c`: local struct array `ps[4]`, global struct array `gs[3]`, `q = a`, `%.2f`, `%.0f` -> `q=6`, `p3=16`, `gs=15`, `f=3.14`, `f0=3`, `result=4`, `ir=86`.
- `/bounds.c`: `struct P ps[2]; ps[2].x = 1` -> `bounds`, exit 1.
- Host also passes: `q=6 p3=16 gs=15 f=3.14 f0=3 ret=4` plus bloop/sieve/fib/float/spill/for regressions.
- Windows unchanged: `/bin/csc3` 100 KiB, `/bin/rirvm` 28 KiB. Default 8k image still ships csc + pvm only.
- Still unsupported: index a whole struct array element (`q = ps[1]`), nested structs, struct return, float struct fields, arrays of structs as parameters.
