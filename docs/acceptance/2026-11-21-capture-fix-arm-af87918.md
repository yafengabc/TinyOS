commit: af87918 (tinyos-script); TinyOS user/rcc/thumb2.inc in this commit
change: fix capture() on Thumb-2: move both pipe ends above fd 2 before the
        redirect, and close fd 1 after the spawn instead of restoring a saved
        copy (the shell does the same)
baseline: c2b27ae / 3167e59
target/config: configs/arm-rir.conf (QEMU mps2 Cortex-M3)
board/port/image: QEMU mps2; ESP32-C3/rv32 backend unchanged
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9, gcc 16.1.0
host:
T0: python tools/test.py -> PASS (host/RV32 backend untouched)
T1: NOT RUN this round (only thumb2.inc changed; 18-target build was green in
    the previous capture commit)
T2: QEMU arm, one boot:
    rt-cap                  -> 1 + "hello from ring 3!"/pid line
    ls /bin | wc            -> 56 224 2224
    rt-probe                -> 31
    rt-cap (again)          -> 1 + the child's output
    rt-probe                -> 31
T3: NOT RUN
T4: NOT RUN
logs: build/qemu-* (local); build/test-capturefix.out
notes:
- Root cause: msh closes fd 1 after a pipeline ("back to the console"), so the
  next process starts with fd 1 closed and pipe() legitimately hands out fd 1.
  The old helper then called dup2(write_end, 1), which closed fd 1 - the read
  end it had just been given - so read() failed (-16 in the sentinel run).
  On a fresh boot fd 1 was open, which is why the first capture always worked.
- Fix: after pipe(), any end below 3 is moved with dup()+close(); then
  dup2(write, 1), close(write), exec, close(1).  The console is the default
  again, exactly like the shell's pipeline path.
- Sentinel instrumentation (read -16, dup2 -13, exec -14) confirmed the
  diagnosis; the sentinels are removed from the final helper.
- RV32 was re-tested with the same command sequence and needed no change; its
  helper still saves/restores fd 1 and passed after pipelines as well.
