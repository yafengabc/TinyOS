#!/usr/bin/env bash
# Build the two-address probe for the ESP32-C3 and, if QEMU is around, boot
# both images to see which addresses the ROM and the CPU accept.
#
# The question this answers: the C3 exposes the same SRAM twice, once through
# the instruction bus (0x40380000) and once through the data bus
# (0x3FC80000), offset 0x700000 apart.  Segment load addresses are accepted in
# both, but whether an instruction can be *fetched* from the data-bus alias is
# not something the datasheet states.  If it can, one flat base works for code
# and data and the loader keeps its single relocation base.  If it cannot,
# every task window has to be split across two address spaces.
#
# Output: build/exp-c3/{iram,dram}.bin, each to be written at flash offset 0.
#
#   scripts/esp-c3/alias.sh                 build both images
#   scripts/esp-c3/alias.sh --run           build, then boot both in QEMU
#   TOS_ESP_QEMU=/path/to/qemu-system-riscv32 scripts/esp-c3/alias.sh --run
set -eu

root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$root/build/exp-c3"
src="$root/scripts/esp-c3"
qemu="${TOS_ESP_QEMU:-$HOME/tools/esp-qemu/qemu/bin/qemu-system-riscv32.exe}"

cc="${TOS_CC_TARGET:-clang}"
triple=--target=riscv32-unknown-elf
flags="$triple -march=rv32imc -mabi=ilp32 -mcmodel=medany -ffreestanding -fno-builtin -fno-pic"

mkdir -p "$out"

build() { # name base char
    $cc $flags -nostdlib -DCH=$3 -c "$src/probe.S" -o "$out/$1.o"
    ld.lld -m elf32lriscv -T "$src/probe.ld" --defsym=BASE=$2 -o "$out/$1.elf" "$out/$1.o"
    (cd "$root" && go run ./cmd/tos espimg "$out/$1.elf" "$out/$1.bin" >/dev/null)
    printf '%s: ' "$1"
    python - "$out/$1.bin" "$out/$1-flash.bin" <<'PY'
import sys
data = open(sys.argv[1], 'rb').read()
flash = bytearray(b'\xff' * 0x400000)
flash[:len(data)] = data
open(sys.argv[2], 'wb').write(flash)
print(f"{len(data)} bytes, flash image written")
PY
}

build iram 0x40380000 65   # 'A' through the instruction bus
build dram 0x3FC80000 66   # 'B' through the data bus

if [ "${1:-}" != "--run" ]; then
    echo
    echo "to flash on a board:  esptool.py --chip esp32c3 -p COMx write_flash 0x0 <file>.bin"
    exit 0
fi

if [ ! -x "$qemu" ]; then
    echo "qemu not found at $qemu (set TOS_ESP_QEMU)" >&2
    exit 1
fi

for n in iram dram; do
    echo
    echo "===== $n ====="
    # The image runs forever by design, so the boot log is all there is.
    timeout 8 "$qemu" -machine esp32c3 -nographic \
        -drive "file=$out/$n-flash.bin,if=mtd,format=raw" 2>&1 | head -16 || true
done
