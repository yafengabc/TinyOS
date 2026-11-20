commit: d8913f9
change: csc3 enum constants and switch/case/default, with break/continue scoping across switch and loops
baseline: 4247837
target/config: esp32c3-generic / configs/esp32c3-csc3.conf
board/port/image: ESP32-C3 (COM4); build/tinyos-esp32c3-generic-full.bin
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
T3: ESP32-C3 COM4 -> PASS
T4: NOT RUN
logs: build/t3-csc3-switch-d8913f9.log (local only)
notes:
- `enum { RED, GREEN = 5, BLUE };` values usable in expressions and `case` labels.
- `switch` supports `case`/`default`, fallthrough, and `break`; `continue` inside a switch still targets the enclosing loop, and `break` inside a switch exits the switch rather than the loop.
- Board `switch.c`: `case RED/GREEN/BLUE/default`, fallthrough `case 1 -> case 2`, switch-in-loop break, and continue through a switch. Output `105`, `2`, `3`, `result=105`, `ir=81`.
- Host suite also passes flow/flow2, struct/float, many20 (12.8 KB source, 21 functions), bloop/sieve/fib and bounds.
- Test sources were uploaded with `tosput-cli`; no test program is added to the image.
- Windows: csc3 108 KiB (110592), rirvm 32 KiB. The switch/enum code grew csc3 from 100 to 108 KiB; still fits the C3 task area.
- Limitations: `case` takes an integer literal or enum constant (no case arithmetic); duplicate case values are not diagnosed; switch expression is coerced to int.
- Standalone project commit for the same change: `2bc37b7`.
