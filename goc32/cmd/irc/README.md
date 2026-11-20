# irc — RIR2 to RV32 ELF

`irc` is the native backend for `csc3`'s RIR2 images.  It reuses the goc32 RV32
assembler, ELF linker and LZ4TOS packer, so its output is directly loadable by
TinyOS `exec`.

The RIR2 producer and the interpreter live in the separate `tinyos-script`
project (`src/csc3.c`, `src/rirvm.c`).  `rirvm` remains the differential oracle.

## Usage

```sh
go build ./cmd/irc
./irc input.rir output.tos
```

Options:

```text
-asm file.s   write the generated assembly
-elf file.elf write the raw ELF as well
```

The `.tos` file is the LZ4TOS envelope expected by TinyOS.  On a board, upload
it into `/bin`:

```sh
tosput-cli COM4 output.tos /bin/prog
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
- `FMUL`/`FDIV` with the same Q16.16 results as `rirvm`;
- arena strings (`LOADB`, `STOREB`, `STRCPY`, `STRCAT`, `STRLEN`, `STRCMP`,
  `ATOI`, `PRINTSTR`);
- `bounds` and `arena bounds` traps print the same text and exit 1.

Conditional branches are emitted as an inverted short branch over a `j`
(±1 MiB), so RV32's ±4 KiB B-type range cannot break large functions.

## C port

This Go tool is the reference implementation.  The final `irc` is meant to be
a TinyOS C user program so native compilation can happen on the board (the
PC-side path already has goc32).  The port needs, in order:

1. RIR2 reader and the same lowering table.
2. RV32 instruction encoders plus a label/fixup list (no assembler text).
3. ELF32 `ET_DYN` writer matching `user/user.ld` and `kernel/elf.c`.
4. A stored-LZ4TOS wrapper (one literal run is a valid LZ4 block) so `exec`
   accepts the written file.
5. The same runtime helpers, emitted as bytes with the Go assembly above as
   the pattern.

The register mapping and hidden struct-return argument stay unchanged, so both
backends can be compared on the same RIR2 image.

## Limits

- No libc is linked: `gp` is repurposed as r12.  Do not mix `irc` output with
  objects that assume the standard global pointer.
- `pwm_set` uses one static request buffer, matching the single-threaded RIR2
  execution model.
- One native frame is 64 bytes plus spill slots; recursion depth is bounded by
  the TinyOS stack.
- The source RIR2 image must fit the TinyOS window; `lz4tos` reports the span.
