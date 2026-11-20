commit: 8d15cbc (TinyOS), corpus 9f6d27c in yafengabc/tinyos-script
change: shrink Node from 124 to 56 bytes: identifiers become 64-bit hashes;
        declaration names and dotted field paths go to a per-function text pool
baseline: 945fbe0
target/config: esp32c3-generic / configs/esp32c3-rir.conf; rv32 / configs/rv32-rir.conf
board/port/image: ESP32-C3 COM4; QEMU virt rv32
toolchain: go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9, gcc 16.1.0
host:
T0: go build ./...; go vet ./...; go test ./... -count=1 -> PASS
    python tools/test.py -> PASS, all 44 cases, native images byte-identical
    in text size to the previous commit (bigcode 10128, many100 24232,
    bigir 17292, hugeir 29556)
T1: go run ./cmd/tos build all -> PASS, 18/18 targets
T2: smoke rv32/rv32smp/arm -> PASS 3/3; soak NOT RUN
T3: ESP32-C3 COM4 -> PASS; RP2350 NOT RUN
T4: NOT RUN
logs: build/test-hash.out, build/t3-node-hash.log, build/qemu-hash.log (local)
notes:
- Node was 124 B: 28 control, 64 name/field text, 32 pointers.  It is now 56 B:
  uint64 hash, 8 pointers, int val, uint32 field_off, uint16 alen/line and four
  uint8 kinds.  256 nodes: 31744 -> 14336 B (-17.4 KiB).
- Identifier *references* are 64-bit FNV-1a.  The symbol, function, struct and
  constant tables keep their real names, so `sym_add()` detects a collision
  (equal hash, different string) and stops with
  "identifier hash collision" instead of silently merging.  With the tables
  bounded by FUNC_MAX/SYM_MAX/STRUCT_MAX that is ~1e-14 per program.
- Two things still need text: N_DECL (declare_locals and gen_stmt bind the
  symbol by name) and dotted field paths ("in.y" cannot be split from a hash).
  Both go to a 3 KiB per-function pool reset at `parse_function`, so it cannot
  fill across functions; the bound is SYM_MAX declarations plus the paths one
  function can spell.
- Sym gained an 8 B cached hash (+512 B) so lookups do not rehash the table.
  BSS 71692 -> 57872; text 55504 -> 58061 (+2.5 KiB of hash helpers).
  csc window 143360 -> 131072 (128 KiB); span 122880 -> 110592.
- C3 end-to-end after the change: many32 -> 496, bigcode (10128 text / 780
  labels) -> 12829, bigir (17292 / 2062) -> 1325, strings2 (0/-1/42/-7),
  struct3 (1/4/3/3), nest (3 4 5 7 7 11 12 21 20 21 20 21 22 62).  All match the
  rvm/gcc reference.
- QEMU rv32: bigcode, strings2 and nest compiled and ran on target with the
  same output; `SMOKE TEST OK`.
- Follow-up levers left: `newir` (5 KiB), `loops[16]` (4.2 KiB) and the 16 KiB
  csc stack; together they would take the window to ~110 KiB.
