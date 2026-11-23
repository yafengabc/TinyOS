# rcc — RIR2 to RV32 ELF

`rcc` is the native backend for `csc`'s RIR2 images.  The board-side C
implementation now lives in `tinyos-script/src/rcc.c` and is pinned as TinyOS
`user/rcc.c`; this Go program remains the reference lowering and the
convenient host-side generator (it reuses goc32's assembler, linker and
LZ4TOS packer).

## Usage

```sh
go build ./cmd/rcc
./rcc input.rir output.tos
```

Options:

```text
-asm file.s   write the generated assembly
-elf file.elf write the raw ELF as well
```

The `.tos` file is the LZ4TOS envelope expected by TinyOS.  On a board, upload
it into `/bin`:

```sh
tosput COM4 output.tos /bin/prog
prog
```

## Register mapping

RIR2 is already register-allocated.  The backend maps:

| RIR | RV32 | role |
|---|---|---|
| r0..r11 | s0..s11 | callee-saved, persist across RIR calls |
| r12 | gp | callee-saved (freestanding; no libc/gp use) |
| r13..r15 | t0..t2 | per-instruction scratch |

Every RIR function saves `ra`, `s0..s11` and `gp`, allocates space for the
`LOADL`/`STOREL` spill slots, then loads its parameters from the global
`argbuf`.  `CALL` uses a direct native call; the return value moves from `a0`
to the RIR destination.  Struct-returning functions use the existing hidden
destination argument in `argbuf`.

## Implemented RIR2 ops

All 68 opcodes are implemented.  The hardware natives map to the TinyOS
syscall ABI: GPIO/ADC/PWM opcodes, `sleep` and `uptime`; `pwm_set` stages its
request in a small `.bss` buffer.

Runtime helpers in the generated image:

- integer and Q16.16 formatting (`PRINT`, `PRINTV`, `PRINTFIX`, `PRINTFIXV`,
  `PRINTFIXN`);
- `FMUL`/`FDIV` with the same Q16.16 results as `rvm`;
- arena strings (`LOADB`, `STOREB`, `STRCPY`, `STRCAT`, `STRLEN`, `STRCMP`,
  `ATOI`, `PRINTSTR`);
- `bounds` and `arena bounds` traps print the same text and exit 1.

Conditional branches are emitted as an inverted short branch over a `j`
(±1 MiB), so RV32's ±4 KiB B-type range cannot break large functions.

## Measured on ESP32-C3

Same source and workload, timed with the program's own `uptime()` calls:

| workload | csa + pvm | csc + rvm | native rcc |
|---|---:|---:|---:|
| recursive `fib(24)` | 608 ms | 242 ms | 84 ms |
| 10,000,000-iteration loop | 53698 ms | 8286 ms | 578 ms |

Native is 7.2x faster than the original `csa + pvm` pair on recursion and
92.9x faster on the loop.  The recursive case pays for saving/restoring 13 RIR
registers per call; the loop case shows the removal of the interpreter
dispatch.

## C port

The Go tool is the reference implementation; the final board-side C `rcc` now
lives in `tinyos-script/src/rcc.c` (pinned as TinyOS `user/rcc.c`).  It emits
machine code directly, writes an ELF32 `ET_DYN` image and a stored-LZ4TOS
envelope.  The register mapping and hidden struct-return argument are
unchanged, so both backends can be compared on the same RIR2 image.

## Limits

- No libc is linked: `gp` is repurposed as r12.  Do not mix `rcc` output with
  objects that assume the standard global pointer.
- `pwm_set` uses one static request buffer, matching the single-threaded RIR2
  execution model.
- One native frame is 64 bytes plus spill slots; recursion depth is bounded by
  the TinyOS stack.
- The source RIR2 image must fit the TinyOS window; `lz4tos` reports the span.
