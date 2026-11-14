---
name: Hardware round
about: One acceptance round on a board that only the other side has
title: "round: "
labels: hardware
---

<!--
A round is one commit plus one board.  Fill in what you ran and paste the raw
log; the verdict table is what gets read back into docs/OPEN-ITEMS.md, so it
matters more than prose.

Do not restate the criteria here - cite the section.  The document is the
single source of truth; this ticket is the hand-off.
-->

**Commit** (`git log -1 --oneline`):

**Board / port**:

**Images** (delete the ones not run):

| image | backend | flashed | booted without RESET |
| --- | --- | --- | --- |
| `rp2350` | PMSAv8 | | |
| `rp2350smp` | PMSAv8 | | |
| `rp2350rv` | NAPOT | | |
| `rp2350rvsmp` | NAPOT | | |
| `rp2040` / `rp2040smp` | none (software guard) | | |
| `esp32c3` | TOR | | |

**What was run** (commands, one per line):

```
python scripts/rp2350/accept33.py --image ... --uf2 build/...
```

**Raw log**: attach the `build/sizeprobe/accept33-*.log` (drag the file into
this comment - do not paste 500 lines inline).

**Verdict**

| item | criteria (doc section) | result | note |
| --- | --- | --- | --- |
| boot without RESET | OPEN-ITEMS 33.4 | | first byte / prompt latency |
| partition / df | 33.0 item 2 | | total / used % |
| isolation | 33.1, 33.2 | | which `prot` cases faulted |
| stack growth | 33.2.1 | | grown to KiB, journal |
| second core | 33.2 | | smpbench ratio, dd rounds |
| single-core baseline | §33 每轮附加 | | wink matches the SMP twin |

**Failures**

<!--
For each failure: what printed, and which side of the seam you think it is -
kernel, backend, or the test itself.  A failure that reproduces on both boots
is a code bug; one that only shows on the first boot after flashing is usually
state, not logic.
-->

**Conclusions written back to**: `docs/OPEN-ITEMS.md` §
