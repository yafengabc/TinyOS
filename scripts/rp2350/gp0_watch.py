"""Watch /proc/gp0drive and print only when the verdict changes.

Built for one bench test: with the adapter's lead on GP0 unplugged, the pad
should finally read high when SIO drives it.  Sampling in a loop means the
unplug and the re-plug both show up as transitions, which is stronger
evidence than a single reading after the fact - a single reading cannot tell
"the load is gone" from "the wire was never plugged in".

    python scripts/rp2350/gp0_watch.py --port COM27 --seconds 900
"""

import argparse
import time

import serial


def sample(port):
    s = serial.Serial(port, 115200, timeout=0.2)
    s.dtr = True
    s.rts = True
    time.sleep(0.2)
    s.reset_input_buffer()
    s.write(b"cat /proc/gp0drive\r")
    buf = b""
    end = time.time() + 6
    while time.time() < end:
        chunk = s.read(s.in_waiting or 1)
        if chunk:
            buf += chunk
            if buf.rstrip().endswith(b"tos> "):
                break
    s.close()
    text = buf.decode("utf-8", "replace")
    drive1 = ""
    verdict = ""
    for line in text.splitlines():
        if "driving 1" in line:
            drive1 = line.strip()
        elif "verdict" in line:
            verdict = line.strip()
    return drive1, verdict


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM27")
    ap.add_argument("--seconds", type=float, default=900.0)
    ap.add_argument("--out", default="build/gp0_watch.log")
    args = ap.parse_args()

    last = None
    t0 = time.time()
    with open(args.out, "w", encoding="utf-8") as log:
        while time.time() - t0 < args.seconds:
            try:
                drive1, verdict = sample(args.port)
            except Exception as exc:                      # port busy/unplugged
                print(f"{time.strftime('%H:%M:%S')} no answer: {exc}")
                time.sleep(2.0)
                continue
            now = drive1 + " | " + verdict
            if now != last:
                stamp = time.strftime("%H:%M:%S")
                print(f"{stamp} {now}", flush=True)
                log.write(f"{stamp} {now}\n")
                log.flush()
                last = now
            time.sleep(1.0)


if __name__ == "__main__":
    main()
