#!/usr/bin/env python3
"""Repeat the power-on path N times and report, per cycle, whether the board
came back.

    python scripts/rp2350/gate_boot.py --uart COM19 -n 15 --cpus 2 \
        --label rpi-pico2-smp --out build/gate-rpi-pico2-smp.log

Why `reboot` and not a power supply: plat_reboot() goes back through the ROM's
own BOOT_TYPE_NORMAL entry, which is the same path a power-on takes (it
re-scans flash for the IMAGE_DEF and boots this image).  The only thing a bench
power cycle adds is analogue state, and the failure this gate is looking for -
an SMP board that comes up with a dead CDC or a wedged core - is reachable
from here.

Two channels, deliberately:

  * UART0 (the CH343 adapter) does not re-enumerate when the chip resets, so it
    is the reliable witness of "did the kernel boot and reach a shell".
  * the native CDC does re-enumerate, and its two failure modes are different:
    the port never appears, or it appears and cannot be opened (the shape the
    rp2350 SMP wedge took).  A cycle that only watched UART0 would miss it.

So a cycle is a pass when a shell answers on either channel, and the per-channel
result is what gets recorded - "UART silent, CDC fine" is the known RV32
warm-reboot item (OPEN-ITEMS 5b-5), not a boot failure.
"""

import argparse
import os
import re
import string
import sys
import time

import serial
import serial.tools.list_ports as list_ports

PROMPT = "tos>"
FAULT = ("fault", "panic", "TRAP", "trap", "[STALL]", "assert", "KERNEL")


def cdc_ports():
    """Every native-CDC port on the bus (VID:PID 2E8A:000A)."""
    out = []
    for p in list_ports.comports():
        if "2E8A:000A" in (p.hwid or "").upper():
            out.append(p.device)
    return out


class Chan:
    """One serial handle that can be reopened when the peer resets."""

    def __init__(self, port=None, baud=115200, dtr=False):
        self.port = port
        self.baud = baud
        self.dtr = dtr
        self.s = None

    def open(self, port=None):
        self.close()
        if port:
            self.port = port
        if not self.port:
            return False
        try:
            self.s = serial.Serial(self.port, self.baud, timeout=0.05)
            self.s.dtr = self.dtr
            return True
        except Exception:
            self.s = None
            return False

    def close(self):
        if self.s:
            try:
                self.s.close()
            except Exception:
                pass
            self.s = None

    @property
    def alive(self):
        return self.s is not None

    def read(self):
        if not self.s:
            return b""
        try:
            return self.s.read(4096)
        except Exception:
            self.s = None
            return b""

    def drain(self, seconds=0.2):
        end = time.time() + seconds
        while time.time() < end:
            self.read()

    def write(self, text):
        if not self.s:
            return False
        try:
            self.s.write(text.encode() + b"\r\n")
            return True
        except Exception:
            self.s = None
            return False

    def wait_prompt(self, timeout):
        """Read until the log ends in a prompt; returns (ok, text, elapsed)."""
        t0 = time.time()
        buf = bytearray()
        end = t0 + timeout
        while time.time() < end:
            chunk = self.read()
            if chunk:
                buf += chunk
                tail = buf.decode("utf-8", "replace").strip()
                if tail.endswith(PROMPT):
                    return True, buf.decode("utf-8", "replace"), time.time() - t0
        return False, buf.decode("utf-8", "replace"), time.time() - t0

    def sync(self, timeout=6.0):
        """Get to a prompt: a bare CR is enough when the shell is already up."""
        self.drain(0.15)
        self.write("")
        return self.wait_prompt(timeout)


def ask_gp0drive(cdc_port):
    """Ask a booted board whether its TX pad can pull the line high.

    A silent UART0 is ambiguous to every observer on the outside: the board
    may have stopped driving, or the adapter may have stopped listening.
    /proc/gp0drive drives the pad from SIO and reads it back, so this
    distinguishes them - and printing it at gate start means the bench state
    is recorded instead of being guessed at from a run of silent cycles.
    Returns the two lines that matter, or "" when the node is not there.
    """
    c = Chan(cdc_port, dtr=True)
    if not c.open():
        return ""
    c.write("cat /proc/gp0drive")
    _, txt, _ = c.wait_prompt(6.0)
    c.close()
    keep = [l.strip() for l in txt.splitlines()
            if "driving 1" in l or "verdict" in l]
    return " | ".join(keep)


def run_cycle(uart, cdc_port, cpus, timeout, uart_wanted):
    """One boot: reset, then see who answers.  Returns a dict of findings."""
    rec = {
        "uart_boot": False,
        "uart_s": None,
        "cdc_port": None,
        "cdc_open": False,
        "cdc_boot": False,
        "cdc_s": None,
        "cpus": None,
        "uptime_s": None,
        "fault": "",
        "note": "",
    }

    # Send the reset from whichever channel has a shell.  UART0 first: it is the
    # one that keeps working when CDC is the thing under test.
    sent = False
    if uart.alive and uart.sync(4.0)[0]:
        sent = uart.write("reboot")
    if not sent:
        rec["note"] = "uart silent before reset"
        tried = []
        for p in ([cdc_port] if cdc_port else []) + cdc_ports():
            if p in tried:
                continue
            tried.append(p)
            # DTR must be asserted: this CDC drops output while the host reads
            # as "not there", and a False DTR therefore looks exactly like a
            # dead board.  Measured on rpi-pico2-rv32-smp: dtr=False gave an
            # empty read, dtr=True gave the prompt.
            c = Chan(p, dtr=True)
            ok = c.open() and c.sync(3.0)[0]
            if ok:
                sent = c.write("reboot")
                rec["note"] = f"reset sent on {p}"
            c.close()
            if sent:
                break
    if not sent:
        rec["note"] = "no channel answered; cannot reset"
        return rec

    # The chip is resetting: give the console a moment to go quiet so the next
    # prompt we see is the new boot's, not the tail of the old shell.
    time.sleep(0.4)
    uart.drain(0.2)

    # UART0 first, but only if it was answering before the reset: on this board
    # the adapter's lead can leave GP0 held low (OPEN-ITEMS item 5), and waiting
    # the full window for a channel that is known dead just burns the cycle.
    # A silent UART0 is never a gate failure on its own - the CDC leg is the
    # authority - but /proc/gp0drive below says which side lost the line.
    t0 = time.time()
    if uart_wanted:
        ok, text, dt = uart.wait_prompt(min(timeout, 12.0))
        rec["uart_boot"] = ok
        rec["uart_s"] = round(dt, 2) if ok else None
        if any(m.lower() in text.lower() for m in FAULT):
            rec["fault"] = "uart: " + ",".join(
                m for m in FAULT if m.lower() in text.lower())
    else:
        rec["note"] = (rec["note"] + "; uart silent before reset").strip("; ")

    # CDC: does it come back, and can the host open it?  Windows needs a moment
    # between the device appearing and the handle working, so keep retrying
    # until the deadline instead of judging on the first sighting.
    deadline = time.time() + max(0.0, timeout - (time.time() - t0))
    while time.time() < deadline and not rec["cdc_boot"]:
        for p in cdc_ports():
            c = Chan(p, dtr=True)
            if c.open():
                rec["cdc_port"] = p
                rec["cdc_open"] = True
                ok2, text2, dt2 = c.sync(5.0)
                rec["cdc_boot"] = ok2
                rec["cdc_s"] = round(dt2, 2) if ok2 else None
                if ok2:
                    c.write("uptime")
                    _, up, _ = c.wait_prompt(4.0)
                    m = re.search(r"cpus=(\d+)", up)
                    if m:
                        rec["cpus"] = int(m.group(1))
                    m = re.search(r"uptime: ([\d.]+) s", up)
                    if m:
                        rec["uptime_s"] = float(m.group(1))
                    if any(m2.lower() in up.lower() for m2 in FAULT):
                        rec["fault"] = (rec["fault"] + " cdc").strip()
                c.close()
                if rec["cdc_boot"]:
                    break
        if not rec["cdc_boot"]:
            time.sleep(0.3)

    if not rec["cdc_open"]:
        rec["note"] = (rec["note"] + "; CDC never opened").strip("; ")

    # UART0 was expected to answer and did not: ask the board which side of
    # the wire is at fault.  /proc/gp0drive drives the pad from SIO and reads
    # it back, so "the pad cannot be pulled high" (adapter/lead) and "the pad
    # pulls high but the UART did not reach it" (firmware) stop being
    # indistinguishable.
    if uart_wanted and not rec["uart_boot"] and rec["cdc_boot"]:
        for p in cdc_ports():
            c = Chan(p, dtr=True)
            if not c.open():
                continue
            c.write("cat /proc/gp0drive")
            _, txt, _ = c.wait_prompt(6.0)
            c.close()
            for line in txt.splitlines():
                if "driving 1" in line or "verdict" in line:
                    rec["note"] = (rec["note"] + "; " + line.strip()).strip("; ")
            break

    # Fall back to UART for the cpu count when CDC is the broken side.
    if rec["cpus"] is None and rec["uart_boot"] and uart.alive:
        uart.write("uptime")
        _, up, _ = uart.wait_prompt(4.0)
        m = re.search(r"cpus=(\d+)", up)
        if m:
            rec["cpus"] = int(m.group(1))
        m = re.search(r"uptime: ([\d.]+) s", up)
        if m:
            rec["uptime_s"] = float(m.group(1))
    return rec


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uart", default="COM19", help="UART0 port (CH343)")
    ap.add_argument("--cdc", default=None, help="CDC port before the first cycle")
    ap.add_argument("-n", "--cycles", type=int, default=15)
    ap.add_argument("--cpus", type=int, default=None, help="expected cpu count")
    ap.add_argument("--timeout", type=float, default=25.0)
    ap.add_argument("--fresh", type=float, default=12.0,
                    help="max uptime in seconds for a shell to count as a fresh boot")
    ap.add_argument("--label", default="gate")
    ap.add_argument("--out", default=None)
    args = ap.parse_args()

    uart = Chan(args.uart)
    if not uart.open():
        sys.exit(f"cannot open {args.uart}")

    cdc = args.cdc or (cdc_ports() or [None])[0]
    if cdc:
        print(f"CDC on {cdc}, UART0 on {args.uart}, {args.cycles} cycles\n")

    uart_alive = uart.sync(4.0)[0]
    if not uart_alive:
        print("UART0 is silent; using CDC as the reset channel "
              "(OPEN-ITEMS item 5)")
        if cdc:
            why = ask_gp0drive(cdc)
            if why:
                print(f"  bench check: {why}")

    rows = []
    for i in range(1, args.cycles + 1):
        rec = run_cycle(uart, cdc, args.cpus, args.timeout, uart_alive)
        rows.append(rec)
        good = rec["uart_boot"] or rec["cdc_boot"]
        cpus_ok = args.cpus is None or rec["cpus"] == args.cpus
        fresh = rec["uptime_s"] is not None and rec["uptime_s"] <= args.fresh
        if good and not fresh:
            rec["note"] = (rec["note"] + "; shell not fresh (no reset?)").strip("; ")
        verdict = "PASS" if (good and cpus_ok and fresh and not rec["fault"]) else "FAIL"
        print(f"[{i:2d}/{args.cycles}] {verdict} "
              f"uart={'ok' if rec['uart_boot'] else 'silent':6s} "
              f"({rec['uart_s']}) "
              f"cdc={rec['cdc_port'] or '-':7s} "
              f"open={'y' if rec['cdc_open'] else 'n'} "
              f"boot={'y' if rec['cdc_boot'] else 'n'} "
              f"cpus={rec['cpus']} up={rec['uptime_s']} {rec['fault']} "
              f"{rec['note']}")
        # Keep the CDC port name for the next cycle: after a reset Windows may
        # hand out a different COM number, and the old handle is dead anyway.
        if rec["cdc_port"]:
            cdc = rec["cdc_port"]
        if rec["uart_boot"]:
            uart_alive = True

    uart.close()

    npass = sum(1 for r in rows
                if (r["uart_boot"] or r["cdc_boot"]) and not r["fault"]
                and r["uptime_s"] is not None and r["uptime_s"] <= args.fresh
                and (args.cpus is None or r["cpus"] == args.cpus))
    print(f"\n{args.label}: {npass}/{len(rows)} cycles reached a shell")
    if rows:
        uart_ok = sum(1 for r in rows if r["uart_boot"])
        cdc_ok = sum(1 for r in rows if r["cdc_boot"])
        never = sum(1 for r in rows if not r["cdc_open"])
        print(f"  uart0 boot {uart_ok}/{len(rows)}, cdc boot {cdc_ok}/{len(rows)}, "
              f"cdc never opened {never}/{len(rows)}")

    if args.out:
        os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(f"# {args.label} gate, {len(rows)} cycles\n")
            f.write("cycle,uart_boot,uart_s,cdc_port,cdc_open,cdc_boot,cpus,"
                    "uptime_s,fault,note\n")
            for i, r in enumerate(rows, 1):
                f.write(f"{i},{int(r['uart_boot'])},{r['uart_s']},{r['cdc_port']},"
                        f"{int(r['cdc_open'])},{int(r['cdc_boot'])},{r['cpus']},"
                        f"{r['uptime_s']},{r['fault']!r},{r['note']!r}\n")
        print(f"log -> {args.out}")


if __name__ == "__main__":
    main()
