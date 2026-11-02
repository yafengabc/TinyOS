#!/usr/bin/env python3
"""Locate the standard TinyOS test runtime.

Order: $TOS_EMU (explicit override), then the binary built from the rv32emu-c
submodule (build/rv32emu-c.exe, see scripts/emu.py). Nothing machine-specific
is baked in; on a fresh clone run `python scripts/emu.py build` first.
"""
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE_NAME = "rv32emu-c.exe" if os.name == "nt" else "rv32emu-c"
LOCAL = os.path.join(ROOT, "build", EXE_NAME)


def find_emu():
    return os.environ.get("TOS_EMU") or LOCAL
