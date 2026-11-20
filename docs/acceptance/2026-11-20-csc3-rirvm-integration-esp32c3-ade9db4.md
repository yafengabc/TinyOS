commit: ade9db4
change: add the experimental csc3 + rirvm register-IR pair behind [features] csc3; default images keep csc + pvm
baseline: 695db5b
target/config: esp32c3-generic / configs/esp32c3-csc3.conf and configs/esp32c3-8k.conf
board/port/image: ESP32-C3 (COM4); build/tinyos-esp32c3-generic-full.bin for each config
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS (no skips added)
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm (ps, ls /bin | wc, cat /proc/execfmt, piperace, prot stack, deep 3, rselftest) -> PASS 3/3; soak NOT RUN
T3: ESP32-C3 COM4 -> PASS (csc3/rirvm image and default 8k image)
T4: NOT RUN
logs: build/t1-build-all.log, build/t3-csc3-ade9db4.log, build/t3-default-8k-ade9db4.log (local only)
notes:
- csc3/rirvm are in user/ but listed in defaultUserExclude; [features] csc3 = true removes them from the exclude set. configs/esp32c3-csc3.conf uses csc3=true and scripting=false.
- csc3 config: /bin has csc3 (17863 B, 100 KiB window) and rirvm (3247 B, 28 KiB window). Demo source written on the board with echo, then `csc3 /d.c /d.rir` and `rirvm /d.rir` -> `result=45 ir=11`.
- default 8k image: /bin still has csc (18217 B) and pvm (10715 B), and no csc3/rirvm, so shipped images are unchanged.
- csc3 limits on embedded targets: 4 KiB source, 128 AST nodes, 256 IR instructions, 24 virtual registers, 8 struct types, 16 functions. Larger inputs fail cleanly.
