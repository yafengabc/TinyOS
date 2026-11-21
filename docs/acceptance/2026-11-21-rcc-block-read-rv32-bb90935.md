commit: bb90935 (tinyos-script); TinyOS user/rcc/rcc.c in this commit
change: rcc reads the RIR2 image in 4 KiB blocks instead of holding the whole
        file in a 40 KiB static buffer; the function table (<=1 KiB), string
        pool (1 KiB) and arena (512 B) are copied once.  RIR_MAX stays as an
        input-size check, so the accepted images are exactly the same set.
baseline: 6924174 / fa81276
target/config: configs/rv32-rir.conf, configs/arm-rir.conf (QEMU)
board/port/image: QEMU rv32 (virt) + QEMU mps2-an385 (Cortex-M3); no hardware
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9, gcc 16.1.0
host:
T0: python tools/test.py -> ALL TESTS PASS (40 program cases)
    byte-identical check: rcc before/after on build/capture1 -> both 3174 B,
    identical bytes (compared in python, not by eye)
T1: go run ./cmd/tos build all -> 18/18 targets
    (first attempt failed on rp2350-generic and x86 with littlefs
     "No more free space 0x12"; files.local/ still held the 31.6 KiB bench.rir
     staged for the QEMU run and those two small images include it.  After
     cleaning files.local/ the same command was green.  Not a code failure.)
T2: QEMU rv32, one boot:
    rcc /bench.rir /bin/brcc -> 18452 text, 18823 tos, 2265 labels
    brcc                    -> 1351
T2: QEMU arm, one boot:
    rcc /bench.rir /bin/brcc -> 31232 text, 31641 tos, 2540 labels
    brcc                    -> 1351
T3: NOT RUN
T4: NOT RUN
bench (host, gcc -O2, 50 runs each, median before -> after):
    bench-rir  31620 B / 1574 IR / 28 funcs: 13.67 -> 13.97 ms
    bigir      28864 B / 1436 IR           : 13.40 -> 13.70 ms
    hugeir     59112 B / 2948 IR           : 18.69 -> 19.04 ms
    (~2% slower: nine extra read() calls per pass on a 40 KiB image)
memory (configs/rv32-rir.conf, build/user/rcc/rcc.o):
    text 35777 -> 36421   data 4 -> 8   bss 71852 -> 37556
    span 106496 -> 69632  window 126976 -> 90112 (88 KiB)
notes:
- The two passes are sequential, so a sliding window never has to be
  invalidated inside a pass; file offsets are absolute, so the second pass just
  seeks and reads again (no cache flushes to get wrong).
- The 20-byte instruction record can straddle a block boundary; rir_read()
  loops until the request is satisfied, so rir_u32() always sees four
  contiguous bytes.
- readdir/isdir and capture() are untouched; T2 ran the normal shell, plus the
  on-target csc -> rcc -> exec chain in the previous card.
- The remaining 88 KiB is dominated by label_off (16 KiB, int), label_seen
  (4 KiB), pc_label (4 KiB) and the four func_* arrays (4 KiB).  Narrowing the
  first three to u16/bitset should take another ~15 KiB if it is ever needed.
