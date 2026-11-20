commit: 3e2bac2
change: split rcc into a shared driver (`rcc.c`) and an architecture-selected RV32 backend (`rv32.inc`)
baseline: 66dc83d
target/config: esp32c3-generic / configs/esp32c3-rir.conf
board/port/image: ESP32-C3 (COM4)
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
    python tools/test.py -> PASS, all 18 native images
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
T3: ESP32-C3 COM4 -> PASS
T4: NOT RUN
logs: build/test-split.out, build/t3-rcc-split.log (local)
notes:
- `user/rcc.c` is now `user/rcc/rcc.c` plus `user/rcc/rv32.inc`.
  `rcc.c` owns RIR2 reading, analysis, labels/two-pass, ELF/LZ4TOS and data
  layout.  `rv32.inc` owns register mapping, encoders, lowering, helpers and
  `rcc_arch_*`.
- The include is selected by architecture; on RV32 it defines
  `rcc_arch_machine() = 243`, `rcc_arch_flags() = 0`.  An ARM build will add
  `thumb2.inc` / `thumb1.inc` with the same `rcc_arch_*` hooks.
- RV32 output is unchanged: `fold.rir` still produces 2328 text / 2592 ELF /
  2632 LZ4TOS; board `csc -> rvm -> rcc -> splitfold` prints 14/3.
- Board `rcc` window remains 73728 bytes (72 KiB); `/bin/rcc` is 27846 bytes
  after the directory build.
- Standalone project commit: `c4c487a`.
