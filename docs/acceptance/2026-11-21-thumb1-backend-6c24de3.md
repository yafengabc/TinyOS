commit: 6c24de3 (tinyos-script; stage 1 was 09f2e2a); TinyOS user/rcc/thumb1.inc
change: finish the ARMv6-M backend: strings, Q16.16 floats, capture()
baseline: 04a31a4 / 05dbe8a
target/config: configs/arm-rir.conf (QEMU mps2-an385 executes the v6-M subset)
board/port/image: QEMU mps2 only; RP2040 has no QEMU model, so T3 is NOT RUN
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9, gcc 16.1.0
host:
T0: python tools/test.py -> ALL TESTS PASS (RV32 backend unchanged)
T1: go run ./cmd/tos build all -> 18/18 targets
T2: QEMU mps2, all interpreter-comparable programs, grouped in four boots:
    44/44 print exactly the rvm lines (integer, loops, calls, globals, arrays,
    bounds, strings, floats, natives), including bigcode 535 IR, bigir 1436 IR,
    hugeir 2948 IR, many100 111 functions
    target-only, one boot: capture1 -> 0, dir1 -> 1/0/-1, file1 -> 1 24 0 0,
    exec1 -> "hello from ring 3!"/pid + 0  (same shape as the Thumb-2 images)
T3: NOT RUN (no RP2040 image or BOOTSEL path yet)
T4: NOT RUN
static check: a table-driven ARMv6-M disassembler (build/thumb1-dis.py) decodes
    the code segment of all 48 generated images with zero unknown encodings;
    BL is the only 32-bit instruction emitted
notes:
- Thumb-1 differences that shaped the port: r0..r7 only for bases and ALU,
  movs/lsls/adds constant builder, `b<inv> +4; bl target` for every branch,
  software divide and a 32x32->64 multiply, RIR 0..3 in r4..r7.
- The one jump that cannot use BL is the branch back into the print helper:
  BL overwrites the return address the helper's push is about to save.  It uses
  a real two-byte B instead (backward, in range).
- Bugs found by the corpus and fixed here: e_mls picking a scratch that was
  also rd/rm/ra; e_la clobbering r3, which helpers use for live values (this
  broke strlcpy/atoi); the software divider clobbering r0 before the caller's
  remainder computation; the fdiv fraction counter starting at 16 so the shift
  became -1.
- Thumb-2 and RV32 output is unaffected: the shared code changes are guarded by
  RCC_ARCH_THUMB1 and RIR_SLOT_BYTES stays 36 for both.
