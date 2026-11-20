commit: 6b13bc1
change: csc3 char arrays, `string` variables, string parameters/returns, and string builtins; RIR2 arena + LOADB/STOREB/STRCPY/STRCAT/STRLEN/STRCMP/ATOI/PRINTSTR
baseline: bdd2b17
target/config: esp32c3-generic / configs/esp32c3-csc3.conf
board/port/image: ESP32-C3 (COM4); build/tinyos-esp32c3-generic-full.bin
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
T3: ESP32-C3 COM4 -> PASS
T4: NOT RUN
logs: build/t3-csc3-str.log, build/t3-csc3-str-regress.log (local only)
notes:
- New `tests/str.c` covers `char buf[N]`, `char c`, global and local char-array literals, `buf[i]` load/store, `buf[0] += 1`, `string` variables and copies, string parameters (`char text[]`), string returns, `print`, `printf("%s")`, `strlen`, `strcmp`, `strlcpy`, `strlcat`, `atoi`, and character literals.
- Board output matches the host, ending `name=Xy`, `n=5`, `result=5 ir=121 reps=1 exec_ms=1`.
- Regressions on the same image: `mod` -> `2/2/2/2`, `result=6 ir=29`; `fold` -> `14/3`, `result=17 ir=14`; `switch` -> `105/2/3`, `result=105 ir=81`.
- Host suite `python tools/test.py` passes all cases including the new `str`.
- Storage: `char[]` bytes and expression string literals live in a 512-byte mutable arena. RIR2 header offset 24 now carries the arena size (`dsize`); old images with 0 are still accepted.
- Limits by design: local `char[]` storage is static arena space (recursion shares it); global `string` initializers and global `char` initializers are rejected; `%` remains integer-only.
- Windows: csc3 span 98304 / window 118784 (was 90112 / 110592); rirvm span 20480 / window 32768.
- Test sources uploaded with `tosput-cli`; no test program in the image.
- Standalone project commit: `5296d83`.
