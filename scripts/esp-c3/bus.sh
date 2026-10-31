#!/usr/bin/env bash
# Build the bus-topology probes for the ESP32-C3 and run them on a board.
#
#   scripts/esp-c3/bus.sh                 build the three images
#   scripts/esp-c3/bus.sh --flash COM4    build, then flash and read each one
#
# The question: the C3 answers at two addresses for the same SRAM, 0x4038_0000
# through the instruction bus and 0x3FC8_0000 through the data bus.  Whether a
# task window can keep one flat base depends on which of fetches and loads may
# cross between them.  Results are written up in busprobe.S; in short, keep
# every address in the 0x4038_0000 range and only fetching is restricted.
set -eu

root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/build/exp-c3"
src="$root/scripts/esp-c3"

cc="${TOS_CC_TARGET:-clang}"
triple=--target=riscv32-unknown-elf
flags="$triple -march=rv32imc -mabi=ilp32 -mcmodel=medany -ffreestanding -fno-builtin -fno-pic"

mkdir -p "$out"

build() { # name ldscript char
    $cc $flags -nostdlib -DMK=$3 -c "$src/busprobe.S" -o "$out/$1.o"
    ld.lld -m elf32lriscv -T "$src/$2" -o "$out/$1.elf" "$out/$1.o"
    (cd "$root" && go run ./cmd/tos espimg "$out/$1.elf" "$out/$1.bin" >/dev/null)
    printf '%-10s %s bytes\n' "$1" "$(stat -c%s "$out/$1.bin")"
}

build bus-code  bus-one.ld   0x49   # 'I': both buses reach the 0x4038 range
build bus-data  bus-dram.ld  0x44   # 'D' stored, 0xb7 read back
build bus-fetch bus-fetch.ld 0x46   # 'F', never appears

if [ "${1:-}" != "--flash" ]; then
    echo
    echo "to run on a board:  scripts/esp-c3/bus.sh --flash COM4"
    exit 0
fi

port="${2:?port required, e.g. COM4}"
for n in bus-code bus-data bus-fetch; do
    printf '\n===== %s =====\n' "$n"
    python "$src/run.py" "$port" "$out/$n.bin" 2 >/dev/null || true
    python "$src/capture.py" "$out/last.raw"
done
