commit: 6ed224d (TinyOS), 9fd8826 in yafengabc/tinyos-script
change: rcc selects rv32.inc / thumb2.inc by architecture; add the Thumb-2
        stage-1 backend (integer core, calls, globals/arrays, print int)
baseline: 46c72c3
target/config: arm / configs/arm-rir.conf (test-only); rv32 regression unchanged
board/port/image: QEMU mps2-an385 (Cortex-M3)
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9, gcc 16.1.0
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
    python tools/test.py -> PASS (RV32 path unchanged)
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
T3: NOT RUN (no ARM board in this run; RP2350 M33 is the closing target)
T4: NOT RUN
logs: build/qemu-thumb-{a,b,c,d,e,final}.log, build/qemu-thumb-fold.log,
       build/test-thumb.out (local, not committed)
notes:
- ABI: RIR 0..6 live in r4..r10, RIR 7..15 in frame slots; arguments travel
  through the shared `data_argbuf`, the return value is r0, helpers use only
  r0..r3/r12 and push r4-r11/lr.  Syscalls are the ARM ABI (r0 = number,
  r1..r3 = args).
- No relocation table: `e_la` does `bl .+4` to put the current PC in LR, clears
  the Thumb bit, then adds the link-time delta with movw/movt.  The image keeps
  the same two PT_LOAD segments as RV32 and the kernel loads it without
  PT_DYNAMIC.  This is why e_la is 5 instructions and always emits movt (pass 1
  and pass 2 must agree on size).
- Branch encoders were derived from clang output for forward/backward and a
  4 MiB offset; conditional branches use Bcc.W and setcc uses the inverted
  condition (the first version was inverted, found by cmp2 printing 0/1
  swapped).
- Bugs found and fixed while bringing it up: setcc inversion; R_LOADA/STOREA
  formed an address with `ldr` instead of `add idx<<2`; R_BOUNDS called the
  bounds helper unconditionally instead of comparing first; LOADL/STOREL used
  the vreg slot range and clobbered spill slots (now 36 + 4*n, frame from the
  shared frame_for); the bounds helper passed 1 in r0 (SYS_write) instead of
  SYS_exit=0 with status in r1.
- Verified on QEMU arm, all against the rvm output for the same RIR2:
  mod 2/2/2/2, flow 26/3/4, flow2 5/30/9, switch 105/2/3, switch2 23/99,
  arith -3/-2/17/9/20/14/2/-14/-2, cmp2 1/0/1/0/1/1/1/1/0/7/0,
  loops2 8/4/10/13, funcs 36/720/1, spill 66/395, deep 325,
  many20, many25 120, many32 496, bigswitch 1836/117/1/-1, bigbody 7432/15,
  arrays2 30/4/13, copy 10/7, nest 14 lines, sret 10 values,
  struct3 1/4/3/3, native 6765, bloop3, sieve3, fib24, ops 21 values.
- On-target chain also checked after the last source copy: `csc /fold.c` ->
  `rcc` -> 14/3, `csc /flow2.c` -> `rcc` -> 5/30/9 on mps2.
- Stage 1 only.  Strings (STRLEN/STRCMP/STRCPY/STRCAT/ATOI/PRINTSTR), Q16.16
  floats and the native syscalls stop with `rcc: thumb2: <op> not implemented
  yet`; ARMv6-M (Thumb-1, RP2040) is not started.  configs/arm-rir.conf is
  test-only and no shipping config enables `rir` on ARM.
- TinyOS `user/rcc/rcc.c` grew the backend selector and now uses the board
  limits for ARM as well (`#if defined(__riscv) || defined(__arm__)`); the host
  build keeps the large tables.
