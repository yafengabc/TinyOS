package build

import (
	"bytes"
	"debug/elf"
	"fmt"
	"io"
)

// codePageBytes is the page the user linker aligns segments to, and the only
// granularity at which the protection map can put a boundary inside a code
// run.  It is not enough to look at window-grain multiples: the RX run ends at
// the page that contains the start of .data (that page becomes RWX, because a
// page cannot be split), and that page start is a page multiple, not a grain
// multiple.  rcc on rpi-pico2-rv32 is the measured case: its code run is
// 0x7000 bytes (28 KiB) and the fault was at 0x6ffe, exactly there.
const codePageBytes = 4096

// instructionStraddle walks an instruction stream and reports the first
// instruction that reaches across a page boundary.
//
// RISC-V allows an access only when a single PMP entry covers all of it, so a
// fetch that covers bytes in two entries fails even when both grant X.  The
// entries are page-granular here (page-granular NAPOT blocks for the RX run,
// a separate entry from the page that holds the first writable bytes), so any
// 32-bit instruction that starts two bytes before a page boundary can straddle
// two of them; without RVC every instruction is four bytes and four-byte
// aligned, which cannot happen.  Checking every page is conservative (some
// page boundaries inside one NAPOT block could not fault) and that is on
// purpose: the alternative needs the exact block decomposition, which depends
// on the runtime window base.
//
// base is the offset the code is linked at inside the window.  The first two
// bits of a halfword give the length: 0b11 means 32-bit, anything else 16.
func instructionStraddle(code []byte, base uint32) (addr uint32, size int, ok bool) {
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
		if va/codePageBytes != (va+uint32(size)-1)/codePageBytes {
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
	for _, s := range segs {
		addr, size, ok := instructionStraddle(s.code, uint32(s.vaddr))
		if !ok {
			continue
		}
		return fmt.Errorf("%s: %d-byte instruction at 0x%08x crosses a 4 KiB page boundary; "+
			"Hazard3 splits the code run at page granularity and a fetch over two PMP entries "+
			"faults, so user code must be built without RVC", prog, size, addr)
	}
	return nil
}
