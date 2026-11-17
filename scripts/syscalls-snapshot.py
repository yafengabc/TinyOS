#!/usr/bin/env python3
"""Refresh goc32's copy of the TinyOS syscall table.

goc32 must build without a TinyOS checkout, so the table it was written
against lives in the goc32 repository as a source file (ARCHITECTURE-NEXT
3.1).  This script is the only writer: it copies abi/syscalls.csv verbatim
into goc32/abi/syscalls.snapshot.csv and puts the provenance in a comment
header.

  tinyos_commit       the TinyOS HEAD this copy was taken from; provenance
                      only, because the snapshot is committed after it
  tinyos_csv_sha256   sha256 of the copied bytes - this is the check

The hash covers the CSV body, so goc32 can check the file on its own, and
the TinyOS side can check it against abi/syscalls.csv when both trees are
present (internal/build/syscallsnapshot_test.go).

Usage: python scripts/syscalls-snapshot.py [--check]

--check regenerates in memory and compares, exiting 1 on a difference.  That
is what the TinyOS test does; the flag is for a quick manual run.
"""

import hashlib
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV = os.path.join(ROOT, "abi", "syscalls.csv")
SNAPSHOT = os.path.join(ROOT, "goc32", "abi", "syscalls.snapshot.csv")

HEADER = """\
# TinyOS syscall table snapshot - goc32's input, not a build product.
#
# goc32 builds without a TinyOS checkout, so this copy is committed there;
# abi/syscalls.csv in the TinyOS tree stays the single source of truth.
# When both trees are present, internal/build/syscallsnapshot_test.go compares
# the two and fails if they differ.
#
#   tinyos_commit: {commit}
#   tinyos_csv_sha256: {digest}
#
# Regenerate after changing abi/syscalls.csv:
#   python scripts/syscalls-snapshot.py
"""


def head_commit():
    out = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD"],
                         capture_output=True, text=True)
    commit = out.stdout.strip()
    if out.returncode != 0 or not commit:
        raise SystemExit("cannot read HEAD; run this inside the TinyOS tree")
    return commit


def build():
    with open(CSV, "rb") as f:
        body = f.read()
    if b"\r" in body:
        raise SystemExit("abi/syscalls.csv has CRLF; the snapshot is LF-only")
    text = HEADER.format(commit=head_commit(),
                         digest=hashlib.sha256(body).hexdigest())
    return text.encode() + body


def main():
    want = build()
    if "--check" in sys.argv:
        try:
            with open(SNAPSHOT, "rb") as f:
                have = f.read()
        except FileNotFoundError:
            print("missing:", os.path.relpath(SNAPSHOT, ROOT))
            return 1
        if have != want:
            print("stale:", os.path.relpath(SNAPSHOT, ROOT))
            return 1
        print("up to date")
        return 0

    os.makedirs(os.path.dirname(SNAPSHOT), exist_ok=True)
    with open(SNAPSHOT, "wb") as f:
        f.write(want)
    print("wrote", os.path.relpath(SNAPSHOT, ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
