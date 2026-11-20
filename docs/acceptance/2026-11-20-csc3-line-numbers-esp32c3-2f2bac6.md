commit: 2f2bac6
change: csc3 reports source line numbers for parse and common code-generation errors; AST nodes carry their line
baseline: e5f5987
target/config: esp32c3-generic / configs/esp32c3-csc3.conf
board/port/image: ESP32-C3 (COM4); build/tinyos-esp32c3-generic-full.bin
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
T3: ESP32-C3 COM4 -> PASS
T4: NOT RUN
logs: build/t3-csc3-lines.log (local only)
notes:
- `csc3 /badline.c` -> `expression expected at line 3`; `csc3 /badvar.c` -> `unknown variable at line 3`, both on the board.
- Lexer tracks `src_line`; `node()` records the line; `die()` reports the current line and `die_node()` reports the AST node's line for codegen errors (unknown variable/field, array/struct misuse, argument count, bad expression/statement).
- Regression: `switch.c` still prints `105/2/3`, `result=105 ir=81`.
- Test sources uploaded with `tosput-cli`; no test program in the image.
- Windows: csc3 108 KiB, rirvm 32 KiB.
- Limits: errors from `emit()` (RIR full), allocation, and other late compiler passes still report the current line rather than an exact node line. Parser and common semantic/codegen errors are covered.
- Standalone project commit: `808f708`.
