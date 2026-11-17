package build

/* Chip-layer facts: which architectures a chip can run and what the compiler
 * needs to target each.  Boards do not repeat these; board.conf names a chip
 * and the target's arch selects one of the entries below.  Adding a board on
 * an existing chip therefore needs no Go edit; adding a new chip is the only
 * change that belongs here.
 *
 * -march stays on the target: it is an image choice inside the chip's ISA
 * (rv32 and rv32nc run on the same hart).  Triple and -mcpu are silicon
 * facts and live here.
 *
 * The RP2350 is deliberately two ISAs: the same silicon boots Cortex-M33 or
 * Hazard3 RISC-V, and board.conf uses `chip = rp2350` for both. */

// ChipISA is the compiler-facing description of one architecture on a chip.
type ChipISA struct {
	Triple  string // clang target triple
	Machine string // -mcpu value, "" = compiler default
}

// Chip lists the ISAs a chip can run.
type Chip struct {
	Name string
	ISAs map[string]ChipISA // key: arch subdirectory ("arm", "rv32", ...)
}

var Chips = map[string]Chip{
	"pc":        {Name: "pc", ISAs: map[string]ChipISA{"x86": {Triple: "x86-freestanding"}}},
	"qemu-virt": {Name: "qemu-virt", ISAs: map[string]ChipISA{"rv32": {Triple: "riscv32-unknown-elf"}}},
	"qemu-mps2": {Name: "qemu-mps2", ISAs: map[string]ChipISA{"arm": {Triple: "thumbv7m-none-eabi", Machine: "cortex-m3"}}},
	"esp32c3":   {Name: "esp32c3", ISAs: map[string]ChipISA{"rv32": {Triple: "riscv32-unknown-elf"}}},
	"rp2040":    {Name: "rp2040", ISAs: map[string]ChipISA{"arm": {Triple: "armv6m-none-eabi", Machine: "cortex-m0plus"}}},
	"rp2350": {Name: "rp2350", ISAs: map[string]ChipISA{
		"arm":  {Triple: "thumbv8m.main-none-eabi", Machine: "cortex-m33"},
		"rv32": {Triple: "riscv32-unknown-elf"},
	}},
}

// boardChips is the construction-time policy: which physical chips a board
// directory may build for.  rp2350rv is the same board as rp2350 with the
// other ISA, so both chip names are allowed there; every other board is
// one-to-one.  A wrong `chip =` in board.conf is caught here instead of
// producing an image with the wrong triple.
var boardChips = map[string][]string{
	"pc":        {"pc"},
	"qemu-virt": {"qemu-virt"},
	"qemu-mps2": {"qemu-mps2"},
	"esp32c3":   {"esp32c3"},
	"rp2040":    {"rp2040"},
	"rp2350":    {"rp2350"},
	"rp2350rv":  {"rp2350"},
	"pico2":     {"rp2350"},
}

// isaFor returns the chip/ISA pair a target should compile for.
func isaFor(chip, arch string) (ChipISA, bool) {
	c, ok := Chips[chip]
	if !ok {
		return ChipISA{}, false
	}
	isa, ok := c.ISAs[arch]
	return isa, ok
}
