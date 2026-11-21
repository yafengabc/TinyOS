commit: 7f4e557 (tinyos-script); TinyOS user/rcc/thumb1.inc in this commit
change: fix Thumb-1 write(fd, buf, len): the length argument was destroyed by
        the encoder's alias path, so write() received the buffer offset
baseline: 861f20d / 5a7ce41
target/config: configs/arm-rir.conf (QEMU mps2; Thumb-1 code runs on the M3)
board/port/image: QEMU mps2 only; RP2040 not re-flashed for this change
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9, gcc 16.1.0
host:
T0: python tools/test.py -> ALL TESTS PASS (host build is RV32, unrelated)
T1: go run ./cmd/tos build all -> 18/18 targets
T2: QEMU mps2, the same .rir through three paths:
      rvm /f1.rir              -> 1 5 0 5 hello 0 0
      t1-file1-th2 (Thumb-2)   -> 1 5 0 5 hello 0 0
      t1-file1-th1 (Thumb-1)   -> 1 5 0 5 hello 0 0   (was 1 / 24 / 0 / <blank>)
T2: the 43-command Thumb-1 corpus on mps2 (four boots) -> no DIFF
T3: NOT RUN
T4: NOT RUN
notes:
- Root cause: emit_native_argbuf for read/write loads fd into r1, buf into r2
  and len into r3, then computes the arena pointer with e_add_reg(2, 0, 2)
  (r2 = r0 + r2).  When rd == rm, e_add_reg used pick_tmp() for a scratch, and
  pick_tmp prefers r3 - the register holding len.  write(fd, arena+buf, 5)
  became write(fd, arena+buf, 24), where 24 is the offset of "hello" in the
  arena.  The kernel wrote 24 bytes and returned 24; the following read then
  hit EOF, which is why file1 printed 24 and an empty line.
- Fix: the alias paths of e_add_reg / e_sub_reg / e_mul push and pop their
  scratch, so a live value in r0-r3 cannot be clobbered any more.  Only the
  rd == rm (or rd == ra) cases pay two instructions.
- Issue #43 listed the host rvm as also returning 24.  That did not reproduce:
  host rvm cannot create "/f1.txt" on Windows (it prints 0/-1/-1/bounds), and
  rvm on mps2 and on the C3 both return 5.  The M33/Thumb-2 result in the issue
  was the correct one.
- file1 stays in the eyeball list: its expected file is produced by the host
  interpreter, which cannot create an absolute path on Windows.  Moving it to
  the strict table needs a host shim that maps "/..." into a temp directory.
