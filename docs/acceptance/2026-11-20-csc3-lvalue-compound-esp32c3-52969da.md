commit: 52969da
change: csc3 compound assignment and ++/-- on struct members, array elements and struct-array members (prefix and postfix)
baseline: da4368c
target/config: esp32c3-generic / configs/esp32c3-csc3.conf
board/port/image: ESP32-C3 (COM4); build/tinyos-esp32c3-generic-full.bin
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
T3: ESP32-C3 COM4 -> PASS
T4: NOT RUN
logs: build/t3-csc3-flow2-52969da.log (local only)
notes:
- `flow2.c` exercises `p.x +=`, `p.x++`, `++p.x`, `a[1] +=`, `a[1]--`, `++a[1]`, `ps[0].x +=`, `ps[0].x++`, `++ps[0].x`, and `a[i] *=`. Board output `5`, `30`, `9`, `result=44`, `ir=99`.
- Host suite also passes flow (`26/3/4`), many20 (12.8 KB source, 21 functions), struct/float, bloop/sieve/fib and bounds.
- Test sources were uploaded with `tosput-cli`; no test program is added to the image.
- Windows: csc3 100 KiB, rirvm 32 KiB. Default 8k image still ships csc + pvm only.
- Known limitation: a compound/inc expression that contains a function call or other side effect in the index is evaluated twice (`a[f()] += 1`); plain variables/constants are the intended use. `%=` is still rejected.
- Standalone project commit for the same change: `312badc`.
