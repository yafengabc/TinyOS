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
| goc32 | goc32 and native32 `go test`, a sample compile, and the QEMU single-file loop |

Definitions of T0–T4 and the exact commands are in `docs/TESTING.md`.

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
- CI green is not a full regression: CI does not run `go test`/`go vet`, x86,
  hardware, or Pico 2. See `docs/TESTING.md` section 11.

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
