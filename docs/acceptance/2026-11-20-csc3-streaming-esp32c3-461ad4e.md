commit: 461ad4e
change: csc3 streams the source and emits RIR2 per function with a function table; rirvm loads RIR2 by magic, and CALL uses a function id instead of a patched entry PC
baseline: 5b32f05
target/config: esp32c3-generic / configs/esp32c3-csc3.conf
board/port/image: ESP32-C3 (COM4); build/tinyos-esp32c3-generic-full.bin
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
T3: ESP32-C3 COM4 -> PASS
T4: NOT RUN
logs: build/t3-csc3-streaming-461ad4e.log (local only)
notes:
- Source is read through a refill buffer; the 4 KiB source cap is gone. The board compiled /many20.c (12,871 B, 12 KB comment + 20 leaf functions + main).
- AST and RIR are per function: /many20.c has 21 functions and 185 RIR, and `/bin/rirvm` ran it with `result=210`.
- Host also ran a 13,301 B, 31-function, 275-RIR program (`result=465`), past the old whole-program 256 limit. The C3 rirvm still caps total IR at 256, by design for the small VM.
- New format: 28-byte header (`RIR2`, nir, gsize, entry, nfuncs, strsize), IR records, nfuncs*4-byte function table, then strings. CALL operands are function ids, so streaming does not need call-site backpatching.
- No extension is used: `csc3 /many20.c /many20` writes RIR2, and `rirvm /many20` finds the format by magic. Old RIR1 files are intentionally rejected.
- Regressions on the board: struct array/assignment/%.2f (`q=6 p3=16 gs=15 f=3.14 f0=3`), bloop1m `1783293664` in 836 ms, out-of-bounds `bounds`, all via RIR2.
- Windows: `/bin/csc3` 92 KiB, `/bin/rirvm` 32 KiB. Default 8k image still ships only csc + pvm.
- Remaining per-function limit: 128 AST nodes. A main with 30 call statements still hits it; 21 functions fit. Function count on C3 is now 32.
