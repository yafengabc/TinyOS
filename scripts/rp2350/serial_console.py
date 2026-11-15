"""Run shell commands on a board over its serial console and print what came back.

The point of this one is what it does *not* do: it opens the port with DTR and
RTS low, so attaching to a running board does not reset it.  A plain
`serial.Serial(port)` leaves the modem-control lines where the previous user
left them, and the CH343 on the ATK-DNRP2350AM pulls RUN on an RTS edge - which
turns "look at the machine that is already running" into "start a new one and
lose the state I wanted to see".  Every `pio regs` / `pio exec` measurement in
docs/OPEN-ITEMS.md 32 was taken through this script for that reason.

    scripts/rp2350/serial_console.py COM14 "pio regs 0 0" "gpio dev 4"
    scripts/rp2350/serial_console.py COM14 --file cmds.txt

One command per argument, sent as a line, waiting for the `tos> ` prompt after
each.  A command that does not come back to a prompt is reported and makes the
exit status 1, so it can be used in a loop without silently passing.
"""
import sys
import time

import serial

PROMPT = b"tos> "


def read_until(port, needles, timeout):
    """Read until one of `needles` shows up, or time runs out."""
    buf = b""
    end = time.time() + timeout
    while time.time() < end:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            buf += chunk
            for n in needles:
                if n in buf:
                    return buf
    return buf


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: serial_console.py <port> [--timeout s] "
                         "[--file cmds.txt] [command ...]\n")
        return 2

    dev = sys.argv[1]
    args = sys.argv[2:]
    wait = 8.0
    cmds = []
    while args:
        a = args.pop(0)
        if a == "--file":
            with open(args.pop(0), encoding="utf-8") as f:
                cmds += [l.rstrip("\n") for l in f
                         if l.strip() and not l.lstrip().startswith("#")]
        elif a == "--timeout":
            wait = float(args.pop(0))
        else:
            cmds.append(a)

    port = serial.Serial(dev, 115200, timeout=0.05)
    port.dtr = False
    port.rts = False
    time.sleep(0.2)
    port.reset_input_buffer()

    port.write(b"\r")
    at_prompt = read_until(port, [PROMPT], 3.0)
    sys.stdout.write("--- at prompt ---\n" + at_prompt.decode("utf-8", "replace"))

    missed = []
    for c in cmds:
        sys.stdout.write("\n$ %s\n" % c)
        sys.stdout.flush()
        port.write(c.encode() + b"\r")
        out = read_until(port, [PROMPT], wait)
        sys.stdout.write(out.decode("utf-8", "replace"))
        if PROMPT not in out:
            missed.append(c)
            sys.stdout.write("\n!! no prompt after '%s'\n" % c)

    port.close()
    if missed:
        sys.stderr.write("!! no prompt: %s\n" % ", ".join(missed))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
