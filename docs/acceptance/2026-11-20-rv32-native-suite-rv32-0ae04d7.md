commit: 0ae04d7 (build/config), corpus 8c77f50 in yafengabc/tinyos-script
change: stage generated .tos from files.local/, add the rv32 rir image config,
        and expand the host RV32 corpus from 18 to 37 cases
baseline: f0a1116
target/config: rv32 / configs/rv32-rir.conf; esp32c3-generic / configs/esp32c3-rir.conf
board/port/image: QEMU virt rv32; ESP32-C3 COM4
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9, gcc 16.1.0 (host)
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
    python tools/test.py -> PASS, 36 rvm cases + 1 native-only case
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
QEMU native: 37/37 rcc images match the rvm program output
T3: ESP32-C3 COM4 -> PASS for bigcode/many32/mixbig/bigswitch; RP2350 NOT RUN
T4: NOT RUN
logs: build/test-suite2.out, build/qemu-rir-{a,b,c}.log, build/check-native.py,
       build/t3-rcc-big.log (local, not committed)
notes:
- New corpus: arith, cmp2, loops2, switch2, funcs, struct3, strings2, float3,
  arrays2, globals2, spill, deep, mixed, many25, many32, bigswitch, bigbody,
  mixbig (all through csc -> rvm -> rcc), and bigcode (native-only).
- many32 is the function-table boundary: 31 helpers + main = 32 = `FUNC_MAX`,
  255 IR (rvm's limit is 256).  It passes in both arms.
- bigcode is 6055 B of source, 31 functions, 535 IR, 10128 B text, 10454 B
  LZ4TOS.  rvm rejects it by design (`m > IRMAX`), so the host runner keeps it
  in a native-only list and asserts `ir > 256`; its printed value (12829) comes
  from a host gcc build.  QEMU and the C3 both printed 12829.
- float3's expected `-0.6666` was wrong (the test was new in this series and
  never committed): -1.5 / -2.25 = -0.66666... prints as `-0.6667`; host gcc
  agrees.  No other expected value changed.
- bigswitch's first draft read an uninitialised `r` after `case 17:`; gcc and
  rvm disagreed because it was UB.  The test now sets `r = 0` and both print
  1836 / 117 / 1 / -1.
- QEMU procedure: copy the generated `.tos` into `files.local/bin/rt-<name>`
  (gitignored; same-name entries replace `files/`), build with
  `configs/rv32-rir.conf`, run `tostest smoke -target rv32 -commands "rt-..."`.
  The 512 KiB qemu-virt ramdisk does not hold all 37 (141 KiB of .tos), so the
  run was split into three batches of 12 / 10 / 16.
- Output comparison: rvm prints a trailing `result=... ir=...` summary that a
  native image does not, so it is stripped before the diff.  `fib24` prints
  nothing in either arm (the value is the exit status), and the trailing blank
  line of `printf("\n")` is eaten by the next prompt; both were handled in the
  checker rather than by changing the programs.
- Board `csc` limits (NODE_MAX 128 per function, IR_MAX 256) make
  `many25`/`many32`/`bigbody`/`bigcode` fail with `AST full` when compiled on
  the board; those RIR2 files are produced by the host csc and only `rcc` runs
  on the target.  Recorded in OPEN-ITEMS §12.
- RP2350 Hazard3 T3 for this corpus is NOT RUN (no board in this run).
