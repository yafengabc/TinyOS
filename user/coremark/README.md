# CoreMark 1.0 on TinyOS

The benchmark core in this directory (`core_main.c`, `core_list_join.c`,
`core_matrix.c`, `core_state.c`, `core_util.c`, `coremark.h`, `ee_printf.c`)
is EEMBC CoreMark 1.0, Apache-2.0 (see `LICENSE.md` and the headers).
Upstream: https://github.com/eembc/coremark.

The TinyOS-specific files are:

- `core_portme.h`: 2K performance run, `MEM_STATIC`, `SEED_VOLATILE`,
  `HAS_FLOAT 0`, `CORE_TICKS` = milliseconds.
- `core_portme.c`: seeds and `clock_ms()` timing. `coremark <iters>` sets
  the iteration count; no argument auto-calibrates to a ~10 s run.
- `ee_printf.c`: upstream, except `uart_send_char()` writes through the
  TinyOS `putchar()` and `coremark.h` is included relatively.
- `core_main.c`: upstream, plus one `[tos] CoreMark 1.0` line that uses the
  millisecond ticks because the integer-seconds path loses resolution.
  The benchmark algorithms and CRC checks are untouched.

Build and run:

```
go run ./cmd/tos build -target rp2350rv
# then flash build/rp2350rv.uf2 and at the shell:
coremark
coremark 6000
```

The reported CRCs (seed 0xe9f5, list 0xe714, matrix 0x1fd7, state 0x8e3a)
must match; the final CRC depends on the iteration count.
