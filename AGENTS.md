# Regression testing rules (all agents)

This file is normative. Every agent making a change in this repository must
follow it. The detailed matrix, commands and evidence template are in
[`docs/TESTING.md`](docs/TESTING.md).

## Non-negotiables

1. **A test without a log did not run.** Report the exact command, target,
   build config, host toolchain, and the result. "Looks fine" is not evidence.
   Write the result as a summary under `docs/acceptance/` (one file per run,
   named as the directory README says) and link it from the commit; raw logs
   stay local or in a CI artifact, not in git.
2. **Never weaken a test to get green.** Do not delete, comment out, or add a
   `t.Skip` to a failing test. Do not regenerate golden files just to match new
   output. If the expected behaviour really changed, update the test in a
   separate commit and say why.
3. **Refactors start with a baseline.** Record the commit and the full T0/T1/T2
   result before the first change. Compare against that baseline, not against
   memory.
4. **Say what was not run.** Hardware tests that were not performed are
   `NOT RUN`, not `PASS`. Build-only and QEMU-only are valid results if labelled.
5. **Do not touch local state.** Never commit `tos.conf`, `scripts/tmp-*.py`,
   `build/`, or `tmp/`. Do not flash a board that is not the target of the
   change.

## Minimum by change type

| Change | Must run |
|---|---|
| docs only | T0 (build + tests) |
| build system / target / config | T0 + T1 (all buildable targets) + T2 smoke |
| kernel, scheduler, process, locks, tty/console | T0 + T1 + T2 (smoke + soak) + T3 ATK SMP, plus C3 if the path is shared |
| arch / board / port | T0 + T1 + T2 for that architecture + T3 on that board |
| drivers | T0 + T1 + T2 if emulated + T3 on the board with that device |
| userland / libc / shell | T0 + T1 + T2 smoke + T3 where the console is involved |
| LZ4TOS / ELF / ABI / protocol | T0 + T1 + T2 full + compatibility vectors + T3 |
| shared headers / `user/lib.h` | T0 **including the toscc sample compile**, plus the rows above that the change touches |
| toscc (`goc32/`) | toscc and native32 `go test`, a sample compile, and the QEMU single-file loop |

Definitions of T0–T4 and the exact commands are in `docs/TESTING.md`.

## Hardware acceptance policy (2026-11-19)

- **RP2350 Pico 2 (RP2350A, 8 MiB), both Cortex-M33 and Hazard3 RV32 SMP**
  (`rpi-pico2-smp` / `rpi-pico2-rv32-smp`), is the closing gate for issue
  acceptance. An issue that needs hardware closes on those two images.
  Pico 2 replaced the ATK board as the test vehicle: 8 MiB of flash makes
  high-intensity soak/dd runs practical, and the board is cheaper. ATK stays
  supported and its images are still built, but it is no longer the gate.
- **ESP32-C3 is tested by the implementation side.** Record it under
  `docs/acceptance/`; it is not delegated and does not need a separate issue.
- **RP2040 (`rpi-pico` / `rpi-pico-smp`) is part of the T3 matrix again
  (2026-11-23).** A change to `arch/arm`, a board, a driver, the kernel, the
  console or a shared header must show `scripts/rp2040/accept.py` on the image
  it affects, run **over the board's own USB CDC console** (2e8a:000a) - the
  UART0 path does not cover the USB one. BOOTSEL is still a physical button
  press: that is the one manual step, not a reason to skip the run. Closing an
  issue that changes one of those paths therefore includes this row; work that
  is RP2350-specific still closes on the Pico 2 images alone.
- Images are board-specific: never present an ATK run as Pico 2 evidence or
  the reverse. The same silicon is not the same board (LED/pins/flash).
- **Soak budget.** T4 runs only on the SMP image of each ISA (`rpi-pico2-smp`,
  `rpi-pico2-rv32-smp`, `rpi-pico-smp`): 10 minutes per release, 30 minutes
  once for the scheduler/kernel generation gate.  Single-core images are
  covered by the T3 accept (dd/rm, windows, segments), not by T4.  A
  10-minute run that reboots or fails escalates to 30.

## Hard rules for agents

- Run `git status --short` before editing and keep unrelated user changes.
- Use `-count=1` for Go tests. Never rely on a cached green result.
- For a refactor, add characterisation tests **before** moving code. Do not
  change both the code and the expected output in the same commit.
- Do not call a PR "tested on hardware" unless the hardware log is attached
  (port, board, image, commands).
- Do not flash an ATK-DNRP2350AM image to a Pico 2. Pico 2 support is a separate
  open item (`docs/OPEN-ITEMS.md` §1b); official MicroPython is the current
  hardware sanity check there.
- When a test fails, first check whether it also fails on the baseline commit.
  A pre-existing failure is recorded in `docs/OPEN-ITEMS.md`, not silently
  ignored.
- CI green is not a full regression: it runs `go test`/`go vet`, the seven
  release images, one read-only build and QEMU smoke on rv32/rv32smp/arm/x86,
  but not hardware, Pico 2, rv32nc, or x86 release artifacts. See
  `docs/TESTING.md` section 11.

## Before you commit

- [ ] T0 is green and the skip list did not grow.
- [ ] The T1/T2/T3 levels required above were run.
- [ ] The run is summarised under `docs/acceptance/` (one file per run) and
      indexed in its README; raw logs are not committed.
- [ ] Logs and the exact commands are in the change description.
- [ ] No test was weakened, skipped, or golden-updated without a separate
      explanation.
- [ ] If hardware was required and unavailable, the result says `NOT RUN` and
      the blocker is recorded in `docs/OPEN-ITEMS.md`.
