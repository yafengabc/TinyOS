commit: 66dc83d
change: rename toolchain to csc/rvm/rcc; old compiler becomes csa; remove the experimental user/rvm; move avm to tinyos-script/legacy
baseline: 6d54b5a
target/config: esp32c3-generic / configs/esp32c3-rir.conf
board/port/image: ESP32-C3 (COM4)
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
    goc32: go test ./cmd/rcc -count=1 -> PASS
    tinyos-script: python tools/test.py -> PASS, all 18 native images
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
T3: ESP32-C3 COM4 -> PASS
T4: NOT RUN
logs: build/t3-rename-csa.log, build/t3-rename-rir.log (local only)
notes:
- Final names: `csc` = C->RIR2, `rvm` = RIR2 VM, `rcc` = RIR2->RV32 ELF.
  Old compiler is `csa` (C->AMX); `pvm` is unchanged.  `csa` + `pvm` are
  kept temporarily as the comparison baseline and are not the final toolchain.
- `user/csc3` moved to `user/csc`, `user/rirvm` to `user/rvm`, `user/irc.c`
  to `user/rcc.c`; old `user/csc` moved to `user/csa` and its messages now use
  `csa`.
- The old hand-written `user/rvm` experiment (own bytecode, hand-coded
  bench/sieve) was removed.  `user/avm` moved to
  `tinyos-script/legacy/avm/` as a non-built backup.
- Feature gate `[features] csc3` is now `[features] rir`; config
  `configs/esp32c3-csc3.conf` is now `configs/esp32c3-rir.conf`.
  `defaultUserExclude` excludes `csc`, `rvm`, `rcc`; default images ship
  `csa` + `pvm`.
- Board check, old pair: `csa /fold.c /fold.amx` -> `pvm /fold.amx` prints
  143 (14 and 3 concatenated, as pvm's integer print has no separator).
- Board check, new path: `csc /fold.c /fold.rir`, `rvm /fold.rir` prints
  14/3; `rcc /fold.rir /bin/nfold`, `nfold` prints 14/3.  `str.c` through
  `csc -> rvm` ends `result=5 ir=118`; `rcc` produces a 4913-byte LZ4TOS and
  the native output matches the full string test.
- On-board sizes: `/bin/csc` 36050, `/bin/rvm` 4851, `/bin/rcc` 18035.
  Windows: csc 126976, rvm 32768, rcc 73728.
- Standalone project commit: `c0abb40`.
