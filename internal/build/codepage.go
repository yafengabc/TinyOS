package build

import (
	"bytes"
	"debug/elf"
	"fmt"
	"io"
)

// codePageBytes is the page the user linker aligns segments to.  Protection
// block boundaries are every window-grain multiple, which for a 4 KiB-grain
// port is exactly one page; the tests use this constant as their base.
const codePageBytes = 4096

// instructionStraddle walks an instruction stream and reports the first
// instruction that reaches across a protection-block boundary.
//
// RISC-V allows an access only when a single PMP entry covers all of it, so a
// fetch that covers bytes in two entries fails even when both grant X.  With
// RVC a 32-bit instruction can start two bytes before a boundary and one fetch
// then covers two entries; without RVC every instruction is four bytes and
// four-byte aligned, which cannot happen.
//
// The boundaries are every multiple of `grain`, which is what the port's map
// produces: pmp_cover() starts at a grain-aligned window base and emits
// naturally aligned power-of-two blocks of at least one grain, so its block
// boundaries can only fall on grain multiples (a smaller remainder block is
// always last).  A code run that fits inside one grain therefore has no
// boundary to cross at all.  Grain comes from the board's [window] grain, so a
// 4 KiB-grain port checks every page and a 16 KiB-grain port only every 16 KiB.
//
// base is the offset the code is linked at inside the window.  The first two
// bits of a halfword give the length: 0b11 means 32-bit, anything else 16.
func instructionStraddle(code []byte, base, grain uint32) (addr uint32, size int, ok bool) {
	if grain < 8 {
		grain = 8
	}
	for off := 0; off+2 <= len(code); {
		hw := uint32(code[off]) | uint32(code[off+1])<<8
		size = 2
		if hw&3 == 3 {
			size = 4
		}
		if off+size > len(code) {
			return 0, 0, false
		}
		va := base + uint32(off)
		if va%grain+uint32(size) > grain {
			return va, size, true
		}
		off += size
	}
	return 0, 0, false
}

type execSegment struct {
	vaddr uint64
	code  []byte
}

// executableSegments returns every PT_LOAD that grants execute permission.
func executableSegments(data []byte) ([]execSegment, error) {
	f, err := elf.NewFile(bytes.NewReader(data))
	if err != nil {
		return nil, err
	}
	var out []execSegment
	for _, p := range f.Progs {
		if p.Type != elf.PT_LOAD || p.Flags&elf.PF_X == 0 || p.Filesz == 0 {
			continue
		}
		body, err := io.ReadAll(io.LimitReader(p.Open(), int64(p.Filesz)))
		if err != nil {
			return nil, err
		}
		out = append(out, execSegment{vaddr: p.Vaddr, code: body})
	}
	return out, nil
}

// checkCodePages rejects a user program whose instructions cross a page
// boundary on a port that splits one permission run into page-granular PMP
// entries (Hazard3 / RP2350 RV32).
//
// This is the pre-board gate for the fault that cost a board session: `rcc`
// died with "instruction access fault" at 0x20026ffe, an instruction two bytes
// before a page boundary inside .text, because the RX run was covered by three
// NAPOT blocks and one fetch covered two of them.  The port builds user code
// without RVC so this cannot happen; the check makes sure it stays that way (a
// hand-written .S with c.* or a stray -march fails the build instead of the
// board).
func (b *Builder) checkCodePages(prog string, data []byte) error {
	// Only a board that asked for the no-RVC policy has this invariant to
	// keep, and the policy is one board fact: "" means the port's map cannot
	// split a code run (TOR: QEMU, the C3), so a straddle is harmless there.
	// With "auto" the build has already relinked the offenders, so this is
	// the gate that proves the fallback worked.
	if b.Target.Arch != "rv32" || b.userRVCMode() == "" {
		return nil
	}
	segs, err := executableSegments(data)
	if err != nil {
		return fmt.Errorf("%s: %w", prog, err)
	}
	grain, _, _ := b.Target.WindowGeometry()
	for _, s := range segs {
		addr, size, ok := instructionStraddle(s.code, uint32(s.vaddr), uint32(grain))
		if !ok {
			continue
		}
		return fmt.Errorf("%s: %d-byte instruction at 0x%08x crosses a 4 KiB page boundary; "+
			"Hazard3 splits the code run at page granularity and a fetch over two PMP entries "+
			"faults, so user code must be built without RVC", prog, size, addr)
	}
	return nil
}
