#!/usr/bin/env python3
"""Pico 2 (rpi-pico2) six-item heartbeat — the B2 deliverable from issue #25.

    python scripts/rp2350/pico2_heartbeat.py            # auto port, rpi-pico2
    python scripts/rp2350/pico2_heartbeat.py -p COM19   # UART0 via the CH343
    python scripts/rp2350/pico2_heartbeat.py -p COM17 --dtr   # board's own CDC

Same six checks and the same `HEARTBEAT OK n/n` / exit-code contract as
scripts/rp2040/heartbeat.py; the implementation lives in scripts/rp2350/
heartbeat.py so the ATK and the Pico 2 cannot drift apart.  With `-p auto`
(the default) the board's USB CDC (2e8a:000a) is preferred and the CH343
UART adapter is the fallback; with two boards attached, name the port.

`--target` defaults to rpi-pico2 here; the other rpi-pico2 images
(-smp, -rv32, -rv32-smp) take --target on the command line.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import heartbeat  # noqa: E402

if __name__ == "__main__":
    sys.exit(heartbeat.main(default_target="rpi-pico2"))
