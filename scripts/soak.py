#!/usr/bin/env python3
"""Window/segment soak: does the task area come back after churn?

`plat_task_protect()` now takes a segment list that `apply_protection()`
rebuilds on every switch, and every exec builds a window out of grains.  Both
are easy to get subtly wrong in a way no single-boot test sees: a stale third
segment, a stack_low that no longer matches after a growth, a window that is
allocated and never handed back.  So this runs the same load over and over on
a QEMU target and compares `free` before and after - the numbers have to be
identical, not merely plausible.

    scripts/soak.py --targets x86,rv32,arm,rv32smp --cycles 6

Each cycle execs programs of different sizes (so the grain allocator sees
several window shapes), grows a heap, grows a stack, and pipes 64 KiB through
a pair of tasks.  Per cycle it also asserts the protection is still there:
the stack probe must fault on the ports that have an MPU/PMP, and the stack
that was grown must still overflow into a fault rather than a silent write.
"""

import argparse
import re
import subprocess
import sys

# Per target: how many protection faults one cycle must produce.
#
# Stack *overflow* is not asserted here: every QEMU board gives tasks a
# 32 KiB stack and `deep` stops at 6 levels (12 KiB) before anywhere near
# full, so there is nothing to kill.  The kill path is covered on hardware
# instead (RP2350 accept33.py `deep 3`).
TARGETS = {
    "x86":     dict(faults=0),   # no paging: prot survives by design
    "rv32":    dict(faults=2),   # prot stack + prot null must fault
    "rv32smp": dict(faults=2),
    "arm":     dict(faults=2),   # ARMv7-M: prot stack + prot null
}

CYCLE = ["deep 3", "mem", "wheap", "piperace", "ls /bin | wc",
         "prot rodata", "prot stack", "prot null", "ps"]


FREE = re.compile(
    r"kernel heap\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+).*?"
    r"SRAM\s+(\d+)\s+(\d+)\s+kernel\s+(\d+)\s+(\d+)\s+"
    r"tasks\s+(\d+)\s+(\d+)\s+unused\s+(\d+)\s+(\d+)", re.S)


def free_state(text):
    """Every `free` report as one tuple, minus the `peak` column.

    peak is a high-water mark and legitimately stays high; everything else
    (heap used, task-area bytes, unused) has to come back to where it was.
    """
    return [m.groups()[:3] + m.groups()[4:] for m in FREE.finditer(text)]


def run(target, cycles, commandwait_ms):
    spec = TARGETS[target]
    cmds = ["free"] + CYCLE * cycles + ["free", "ps", "uptime"]
    out = subprocess.run(
        ["go", "run", "./cmd/tostest", "smoke", "-target", target,
         "-commands", ", ".join(cmds),
         "-commandwait-ms", str(commandwait_ms)],
        capture_output=True, text=True).stdout
    text = out

    problems = []
    if "SMOKE TEST OK" not in text:
        problems.append("smoke run did not finish")
    if "KERNEL PANIC" in text or "System halted" in text:
        problems.append("kernel panic")

    grown = text.count("deep: ")
    if grown < cycles:
        problems.append("deep ran %d/%d times" % (grown, cycles))
    intact = text.count("journal intact")
    if intact < cycles:
        problems.append("journal intact %d/%d" % (intact, cycles))

    raced = text.count("piperace: ok")
    if raced < cycles:
        problems.append("piperace ok %d/%d" % (raced, cycles))

    faults = text.count("access fault") + text.count("MemManage")
    # Each cycle has one prot stack (hole) and one prot null; rodata is not
    # asserted because the available ports disagree on it by design.
    want = spec["faults"] * cycles
    if faults < want:
        problems.append("protection faults %d, want >= %d" % (faults, want))

    rows = free_state(text)
    # Two reports: one before the churn and one after.  Everything except the
    # heap's peak has to be identical.
    if len(rows) < 2:
        problems.append("could not parse both free reports")
    else:
        before, after = rows[0], rows[-1]
        if before != after:
            problems.append("free drifted %s -> %s" % (before, after))

    tasks = re.search(r"tasks\s+(\d+)\s+(\d+)", text)
    return problems, text, tasks.group(0) if tasks else "tasks: ?"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--targets", default="x86,rv32,arm,rv32smp")
    ap.add_argument("--cycles", type=int, default=6)
    ap.add_argument("--commandwait-ms", type=int, default=15000,
                    help="per-command output window passed to tostest smoke."
                         "  piperace pushes 64 KiB through a pipe and prints"
                         " nothing while it does it, so a busy host can sit"
                         " quiet past the window and the next command lands in"
                         " the middle of the output (the rp2350 review thread"
                         " hit this at 1500 ms and again at 4000 ms on"
                         " 2026-11-17).  The wait ends as soon as the prompt"
                         " comes back, so the size of this cap costs nothing on"
                         " a host where the commands finish.")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    failed = 0
    for target in args.targets.split(","):
        target = target.strip()
        if target not in TARGETS:
            print("%-8s unknown target" % target)
            continue
        problems, text, tasks = run(target, args.cycles, args.commandwait_ms)
        print("%-8s %s  %s  (%d cycles)"
              % (target, "PASS" if not problems else "FAIL", tasks, args.cycles))
        for p in problems:
            print("         %s" % p)
        if problems:
            failed += 1
        if args.verbose or problems:
            tail = "\n".join(text.splitlines()[-24:])
            print("         --- tail ---\n" + tail.replace("\n", "\n         "))

    print("\n%d/%d targets passed" % (len(args.targets.split(",")) - failed,
                                      len(args.targets.split(","))))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
