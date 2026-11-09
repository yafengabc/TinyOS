#!/usr/bin/env python3
"""Long-run stability and memory-leak soak for the esp32c3 target.

The loop drives the shell through every kernel allocation path a normal
session uses - exec windows, procfs snapshots, pipe buffers, littlefs file
caches and block writes - and samples the kernel heap *at quiescence* after
each cycle, when only pid 1 (sh) is left:

    kernel heap   total      used      free      peak
                  8192         0      8192      1536

A leak is visible as `used` no longer returning to 0, or as `peak` climbing
past the workload's high-water mark.  Task-window leaks are visible as a
`ps` table that keeps a second row, and as `exec` starting to fail.  A reset
is caught by `uptime` going backwards.

    scripts/esp-c3/soak.py -p COM4 --minutes 30
    scripts/esp-c3/soak.py -p COM4 --cycles 200
    scripts/esp-c3/soak.py -p COM4 --minutes 60 --no-reset

Pressing ^C just stops the loop; the CSV/log written so far stay valid.
"""

import argparse
import re
import sys
import time
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parents[2]
MARK = b"tos> "

# Tokens that mean the kernel or the workload broke.  "stack grown" is not
# here on purpose: `deep` growing its stack is the feature working.
BAD = (
    "KERNEL PANIC",
    "System halted",
    "panic",
    "out of memory",
    "out of task windows",
    "cannot allocate",
    "exec: ",
    "corrupt",
    "unhandled",
    "access fault",
    "illegal instruction",
    "assert",
)

FREE_RE = re.compile(
    r"kernel heap\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)")
UPTIME_RE = re.compile(r"uptime:\s+([\d.]+)\s+s\s+\((\d+)\s+ms\)")
DF_RE = re.compile(r"littlefs\s+(\d+)\s+(\d+)\s+(\d+)")


class Board:
    def __init__(self, port, baud=115200):
        self.s = serial.Serial(port, baud, timeout=0.05)
        self.s.dtr = False          # IO9 high -> boot from flash
        self.s.rts = False

    def close(self):
        self.s.close()

    def reset(self, settle=0.2):
        self.s.rts = True           # EN low -> hold in reset
        time.sleep(0.15)
        self.s.reset_input_buffer()
        self.s.rts = False
        time.sleep(settle)

    def read_until(self, timeout):
        buf = bytearray()
        end = time.time() + timeout
        while time.time() < end:
            chunk = self.s.read(4096)
            if chunk:
                buf += chunk
                if buf.rstrip().endswith(b"tos>"):
                    break
        return buf.decode("utf-8", "replace")

    def boot(self, timeout=25.0):
        self.reset()
        return self.read_until(timeout)

    def cmd(self, line, timeout=20.0):
        self.s.write(line.encode() + b"\r\n")
        return self.read_until(timeout)


def parse_free(text):
    m = FREE_RE.search(text)
    if not m:
        return None
    total, used, free, peak = (int(x) for x in m.groups())
    return total, used, free, peak


def parse_ps(text):
    names = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) >= 10 and parts[0].isdigit():
            names.append(parts[-1])
    return names


def parse_uptime(text):
    m = UPTIME_RE.search(text)
    return int(m.group(2)) if m else None


def bad_tokens(text):
    return [b for b in BAD if b in text]


def workload(n):
    """One cycle of allocation-heavy commands.  Everything is foreground:
    the sample after it must see the shell alone."""
    cmds = [
        "hello",
        "echo abc def | wc",
        "cat /proc/uptime",
        "wfile soak.tmp tiny-os-soak",
        "cat soak.tmp",
        "rm soak.tmp",
        "piperace 1 16",
    ]
    if n % 4 == 0:
        cmds.append("mem")
    elif n % 4 == 1:
        cmds.append("wheap")
    elif n % 4 == 2:
        cmds.append("deep 6")
    else:
        cmds.append("run sh etc/demo.sh")
    if n % 8 == 0:
        cmds.append("py demo.py")
    if n % 10 == 0:
        cmds += [
            "dd of=soak.dd bs=512 count=4",
            "dd if=soak.dd",
            "rm soak.dd",
        ]
    if n % 20 == 0 and n:
        cmds.append("samp")
    return cmds


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-p", "--port", default="COM4")
    ap.add_argument("--minutes", type=float, default=30.0,
                    help="soak duration (default 30)")
    ap.add_argument("--cycles", type=int, default=0,
                    help="stop after N cycles instead of a duration")
    ap.add_argument("--no-reset", action="store_true",
                    help="do not pulse EN; talk to a board already up")
    ap.add_argument("--out", default=str(ROOT / "build"),
                    help="directory for the CSV and raw log")
    args = ap.parse_args()

    outdir = Path(args.out)
    outdir.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S")
    csv_path = outdir / f"soak-esp32c3-{stamp}.csv"
    log_path = outdir / f"soak-esp32c3-{stamp}.log"

    b = Board(args.port)
    t_start = time.time()
    try:
        if args.no_reset:
            b.s.write(b"\r\n")
            boot = b.read_until(5.0)
        else:
            boot = b.boot()
        log = open(log_path, "w", encoding="utf-8")
        csv = open(csv_path, "w", encoding="utf-8")
        log.write("=== boot ===\n" + boot)
        log.flush()
        if "tos> " not in boot:
            sys.exit(f"no prompt after boot ({len(boot)} bytes)")

        csv.write("elapsed_s,cycle,uptime_ms,heap_used,heap_peak,"
                  "heap_free,tasks,task_names,errors\n")
        csv.flush()

        baseline = b.cmd("free")
        log.write("=== baseline ===\n" + baseline)
        base = parse_free(baseline)
        df0 = b.cmd("df")
        log.write(df0)
        names0 = parse_ps(b.cmd("ps"))
        print(f"[soak] baseline heap {base}, tasks {names0}")

        prev_up = 0
        resets = 0
        errors = []
        samples = []
        deadline = t_start + args.minutes * 60.0
        n = 0
        last_print = time.time()

        while True:
            if args.cycles:
                if n >= args.cycles:
                    break
            elif time.time() >= deadline:
                break

            cycle_bad = []
            for line in workload(n):
                out = b.cmd(line)
                log.write(f"\n--- cycle {n}: {line} ---\n" + out)
                hits = bad_tokens(out)
                if hits:
                    cycle_bad.append((line, hits, out[-600:]))
                # A command that never returned to the prompt is fatal for
                # the timing, but try one sample before deciding.
                if not out.rstrip().endswith("tos>"):
                    cycle_bad.append((line, ["no prompt"], out[-300:]))
            log.flush()

            fout = b.cmd("free")
            pout = b.cmd("ps")
            uout = b.cmd("uptime")
            log.write(f"\n=== sample {n} ===\n" + fout + pout + uout)
            log.flush()

            f = parse_free(fout)
            tasks = parse_ps(pout)
            up = parse_uptime(uout)
            hits = bad_tokens(fout + pout + uout)
            if hits:
                cycle_bad.append(("sample", hits, fout + pout + uout))

            if up is not None:
                if up + 1000 < prev_up:
                    resets += 1
                    cycle_bad.append(("uptime", ["reset"], uout))
                prev_up = up

            for item in cycle_bad:
                errors.append((n, item))

            if f:
                used, peak, free = f[1], f[3], f[2]
                row = (round(time.time() - t_start, 1), n, up,
                       used, peak, free, len(tasks), "|".join(tasks),
                       len(cycle_bad))
                samples.append(row)
                csv.write(",".join(str(x) for x in row) + "\n")
                csv.flush()

            n += 1
            if time.time() - last_print >= 30:
                last_print = time.time()
                if f:
                    print(f"[soak] {n} cycles, "
                          f"{time.time() - t_start:.0f}s, "
                          f"used={f[1]} peak={f[3]} tasks={len(tasks)} "
                          f"errors={len(errors)}", flush=True)

        # Final state, including the filesystem side.
        fin = b.cmd("free")
        df1 = b.cmd("df")
        lfs = b.cmd("cat /proc/lfs")
        stack = b.cmd("cat /proc/stack")
        log.write("=== final ===\n" + fin + df1 + lfs + stack)
        log.close()
        csv.close()
        b.close()
    except KeyboardInterrupt:
        print("\n[soak] interrupted")
        raise
    finally:
        try:
            b.close()
        except Exception:
            pass

    elapsed = time.time() - t_start
    print(f"\n[soak] done: {n} cycles in {elapsed:.0f}s "
          f"({elapsed / 60:.1f} min)")
    if samples:
        used = [s[3] for s in samples]
        peak = [s[4] for s in samples]
        max_tasks = max(s[6] for s in samples)
        extra = sorted({s[7] for s in samples if s[7] != "sh"})
        print(f"[soak] heap used: min {min(used)} max {max(used)} "
              f"first {used[0]} last {used[-1]} "
              f"nonzero {sum(1 for x in used if x)}/{len(used)}")
        print(f"[soak] heap peak: first {peak[0]} last {peak[-1]} "
              f"max {max(peak)}")
        print(f"[soak] tasks: max {max_tasks} names {extra or ['sh']}")
        print(f"[soak] resets: {resets}, errors: {len(errors)}")
        if errors:
            for cyc, (line, hits, tail) in errors[:5]:
                print(f"  cycle {cyc} `{line}`: {hits}")
                for ln in tail.splitlines()[-5:]:
                    print("    " + ln)
    print(f"[soak] csv {csv_path}")
    print(f"[soak] log {log_path}")
    return 0 if not errors and not resets else 1


if __name__ == "__main__":
    sys.exit(main())
