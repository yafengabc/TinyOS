commit: ee6fcc4 (TinyOS), tinyos-script source in the same change
change: complete the Thumb-2 backend: strings (strlen/strcmp/strlcpy/strlcat/
        atoi/print), Q16.16 float (fmul/fdiv, print/printf formatting) and the
        GPIO/ADC/PWM/sleep/uptime native syscalls
baseline: 6ed224d
target/config: arm / configs/arm-rir.conf (test-only); rv32 regression unchanged
board/port/image: QEMU mps2-an385 (Cortex-M3)
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9, gcc 16.1.0
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
    python tools/test.py -> PASS (RV32 path unchanged)
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
T3: NOT RUN (no ARM board; RP2350 M33 is the closing target)
T4: NOT RUN
logs: build/qemu-thumb-{reg,all,float,float2,float3,big,str*}.log (local)
notes:
- 28 cases compared line by line with rvm: fold flow2 arith cmp2 funcs spill
  deep many32 bigswitch bigbody arrays2 copy nest sret native ops strings2
  str globals2 mixed float3 fmt sa3 floatfold (all identical), plus the
  native-only bigcode 12829 / bigir 1325 / many100 20009.
- Bugs found while completing it:
  1. strlcpy called strlen on r0 still holding dst, so it copied nothing;
  2. strlcat/lcpy reused label ids (`srcok`, `nok2`) allocated in an earlier
     helper, so branches landed in the data section (PC in the arena);
  3. Thumb `SUB` does not set flags: the float digit loops used `sub`+`bge`
     and walked the buffer pointer off the window.  Added `e_subs_imm`.
  4. printf("%.2f", 2.0) printed 2.0 because the trailing-zero trim was
     applied to %.Nf.  The trim belongs to print(float) (L_fix), which now
     trims to at least one decimal; %.Nf keeps exactly N digits.
  5. The ARM data segment must be 32-byte aligned (MPU minimum region):
     the arena used to land in the read-only code region and stores faulted
     with DACCVIOL.  `rcc_arch_data_align()` is 16 on RV32, 32 on ARM.
- Native syscalls compile and follow the ARM ABI (r0 = number, r1-r3 args),
  but no corpus test drives GPIO/ADC/PWM; that is a board item.
- Thumb-1 (ARMv6-M, RP2040) is still not started; RP2350 M33 T3 NOT RUN.
