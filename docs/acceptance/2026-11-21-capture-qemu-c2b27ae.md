commit: c2b27ae (tinyos-script); TinyOS sources in this commit
change: capture(path, buf, cap) runs a program with its stdout on a pipe and
        returns up to cap bytes of that output
baseline: c899586 / 260f2c3
target/config: configs/arm-rir.conf (mps2), configs/rv32-rir.conf
board/port/image: QEMU mps2 Cortex-M3 and QEMU rv32
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9, gcc 16.1.0
host:
T0: python tools/test.py -> PASS, capture1 is target-only
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: QEMU arm, host-built capture1.tos -> 1 + "hello from ring 3!"/pid line
T2: QEMU rv32, host-built capture1.tos -> captured bytes (cap 31)
T2: on-target arm chain `csc /capture1.c` -> `rcc` -> `ncap` -> 0 (FAIL)
T3: NOT RUN
T4: NOT RUN
notes:
- Implementation: pipe -> dup(1) -> dup2(write end, 1) -> close(write end) ->
  exec -> restore fd 1 -> read loop -> close(read end) -> wait.  The child
  inherits fd 1 at spawn, which is why the parent moves its own fd 1 around
  exec() (the same trick msh's $(...) uses).
- Thumb-2 bug found on the way: the read loop used fds[1] instead of fds[0]
  (write end), so read returned 0; fixed.
- RV32 bug found on the way: emit_helpers() never called emit_capture_helper(),
  so the call target was an unbound label and the task jumped into nowhere
  (instruction access fault at 0x800efffe); fixed.
- **Open**: the image built by the on-target `csc`+`rcc` returns 0 from
  capture() while the host-built image of the same program works.  run() and
  the file-IO chain still work on-target.  Tracked in OPEN-ITEMS §14; want the
  next pass to compare the on-target RIR and the emitted helper bytes.
