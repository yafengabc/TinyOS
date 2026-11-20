commit: 149fab2 (TinyOS), 719245a in yafengabc/tinyos-script
change: add `run(path)` to csc/rvm/rcc: spawn the named program, wait for it,
        return its exit code.  The caller keeps running (TinyOS exec() already
        creates a child; wait() returns the status).
baseline: ee6fcc4 / fa205f7
target/config: configs/arm-rir.conf (mps2), configs/rv32-rir.conf
board/port/image: QEMU mps2 Cortex-M3 and QEMU rv32
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9, gcc 16.1.0
host:
T0: python tools/test.py -> PASS, exec1 is target-only (host rvm returns -1)
    go test ./... -count=1 -> PASS
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke arm (exec1 staged) -> child prints, parent prints 0
    rv32 exec1 -> child prints, parent prints 0
T3: NOT RUN (no board in this run)
T4: NOT RUN
logs: build/qemu-run.log, build/qemu-run-rv.log (local)
notes:
- csc: new native id 11 `run`, one string argument (checked, not coerced to an
  int).  rvm: board builds call exec()+wait(); the host build returns -1 because
  it has no child processes, so exec1 is in the runner's target-only list.
- rcc RV32: SYS_EXEC(3) with the arena pointer, then SYS_WAIT(12) when the pid
  is non-negative.  Thumb-2: same through svc with r0=number and r1=path.
- QEMU mps2, on-target chain: `csc /exec1.c` -> `rcc` -> `ne` prints the child's
  3 lines and then `0`.  QEMU rv32 staged .tos prints the same.
- File IO is **not** implemented.  OPEN-ITEMS §14 records the IR blocker:
  NATIVER has two argument slots, and read/write need three plus a result.
  `open/create/close/unlink` fit the existing encoding; `read/write` need either
  a `NATIVEC` opcode (args from data_argbuf) or a hidden result word.
