commit: 5e5edd8
change: csc3 nested struct fields and dotted field paths; struct returns via a hidden destination argument; nested struct assignment
baseline: f2c7ed3
target/config: esp32c3-generic / configs/esp32c3-csc3.conf
board/port/image: ESP32-C3 (COM4); build/tinyos-esp32c3-generic-full.bin
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
T3: ESP32-C3 COM4 -> PASS
T4: NOT RUN
logs: build/t3-csc3-struct.log, build/t3-csc3-struct-regress.log (local only)
notes:
- Struct fields may now be ints or previously defined structs. Each field has a word offset and type; struct size is the sum of field sizes. Local structs no longer consume a scalar vreg.
- Dotted paths (`o.in.x`, `arr[1].p.y`) are resolved by walking the field types at compile time. Assignment works for leaves and for whole nested struct fields (`o.in = t`, `arr[0].p = t`).
- Struct return ABI: the caller passes one hidden extra `ARG` after the declared parameters, holding the destination word base. The callee copies the result there and returns 0. The hidden register is pinned to r[nparams], so it survives register allocation.
- `tests/nest.c`: prints `3/4/5/7`, `7/11/12/21`, `20/21`, `20/21/22`, `62`; ends `result=21 ir=147`.
- `tests/sret.c`: `make`, `shift`, `shift2` (returning another struct call), `line` with two struct params, and `l.a = make(7,8)`; prints `3/4/13/14/23/24/3/14/7/8`; ends `result=17 ir=118`.
- Regressions: `sa3` -> `result=4 ir=88`; `many20` -> `result=210 ir=165`; `str` -> `result=5 ir=118`; `mod` -> `result=6 ir=26`; `fold` -> `result=17 ir=10`; `switch` -> `result=105 ir=74`.
- Struct parameters are passed as a base offset; field writes through a parameter affect the caller's storage (by reference). Struct locals still use static storage, so recursion shares them.
- A struct-returning call must appear in an assignment or a `return`; using it directly as a scalar/print argument is rejected.
- Limits: 8 fields per struct, 64 words per struct, dotted path stored in the 32-byte field name.
- Windows: csc3 span 106496 / window 126976; rirvm span 20480 / window 32768. On-board `/bin`: csc3 36022 bytes, rirvm 4858 bytes.
- Test sources uploaded with `tosput-cli`; no test program in the image.
- Standalone project commits: `31d5394` (nested fields), `64d192c` (struct returns and nested assignment).
