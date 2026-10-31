#!/usr/bin/env bash
# Build and run a C3 probe.
#
#   scripts/esp-c3/probe.sh pmp                 build build/exp-c3/pmp.bin
#   scripts/esp-c3/probe.sh umode --flash COM4  build, flash, capture last.raw
#
# A probe is NAME.S linked with probe_common.S into one segment at 0x4038_0000.
# Everything goes in a single PT_LOAD on purpose: two segments work on the real
# chip but the ROM then prints an extra "load:" line, which is the noise that
# made the bus experiments look like failures.
#
# The capture of the last run lands in build/exp-c3/last.raw; the analysis is a
# separate script so the raw bytes stay available when the decode is wrong.
set -eu

root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/build/exp-c3"
src="$root/scripts/esp-c3"

name="${1:?probe name, e.g. pmp or umode}"
shift || true

cc="${TOS_CC_TARGET:-clang}"
triple=--target=riscv32-unknown-elf
flags="$triple -march=rv32imc -mabi=ilp32 -mcmodel=medany -ffreestanding -fno-builtin -fno-pic"

mkdir -p "$out"

$cc $flags -nostdlib -c "$src/probe_common.S" -o "$out/probe_common.o"
$cc $flags -nostdlib -c "$src/$name.S"         -o "$out/$name.o"
ld.lld -m elf32lriscv -T "$src/probe-flat.ld" -o "$out/$name.elf" \
    "$out/$name.o" "$out/probe_common.o"
(cd "$root" && go run ./cmd/tos espimg "$out/$name.elf" "$out/$name.bin" >/dev/null)
printf '%-8s %s bytes\n' "$name" "$(stat -c%s "$out/$name.bin")"

if [ "${1:-}" != "--flash" ]; then
    echo
    echo "to run on a board:  scripts/esp-c3/probe.sh $name --flash COM4"
    exit 0
fi

port="${2:?port required, e.g. COM4}"
python "$src/run.py" "$port" "$out/$name.bin" 3 >/dev/null || true
echo "captured $out/last.raw"
