package build

import "testing"

// A page is 0x1000.  Build small instruction streams that end just before a
// boundary and check what the walker says about them.

func hw(v uint16) []byte { return []byte{byte(v), byte(v >> 8)} }

// c.nop (0x0001) is 16-bit, lui x0 (0x00000037 with the low bits 0b11) is
// 32-bit.
var (
	op16 = hw(0x0001)
	op32 = []byte{0x37, 0x00, 0x00, 0x00}
)

func TestInstructionStraddle(t *testing.T) {
	// 16-bit instructions filling up to page_end-2, then a 32-bit one that
	// starts there: it covers two pages.
	code := make([]byte, 0, codePageBytes+4)
	for len(code) < codePageBytes-2 {
		code = append(code, op16...)
	}
	code = append(code, op32...)
	addr, size, ok := instructionStraddle(code, 0, 4096)
	if !ok || size != 4 || addr != codePageBytes-2 {
		t.Fatalf("32-bit instruction at page_end-2 not flagged: ok=%v size=%d addr=0x%x",
			ok, size, addr)
	}

	// The same stream with a 16-bit instruction in that slot is legal: it
	// ends exactly on the boundary.
	ok16 := append([]byte{}, code[:codePageBytes-2]...)
	ok16 = append(ok16, op16...)
	if addr, size, ok := instructionStraddle(ok16, 0, 4096); ok {
		t.Fatalf("16-bit instruction at page_end-2 flagged: size=%d addr=0x%x", size, addr)
	}

	// A 32-bit instruction that ends exactly on the boundary is fine.
	fine := append([]byte{}, code[:codePageBytes-4]...)
	fine = append(fine, op32...)
	if addr, size, ok := instructionStraddle(fine, 0, 4096); ok {
		t.Fatalf("instruction ending on the boundary flagged: size=%d addr=0x%x", size, addr)
	}

	// Alignment comes from the linked base: the same bytes are safe when the
	// image is placed two bytes lower.
	if addr, size, ok := instructionStraddle(code, 0, 4096); !ok {
		t.Fatalf("expected a straddle at base 0 (size %d addr 0x%x)", size, addr)
	}
	shifted := append(append([]byte{}, op16...), code...)
	if _, _, ok := instructionStraddle(shifted, 0, 4096); ok {
		t.Fatalf("shifted stream should not straddle")
	}

	// The boundary follows the board's grain: a 32-bit instruction across the
	// 4 KiB line is fine under a 16 KiB grain (the block spans it) and flagged
	// at the 16 KiB line.  This is the "a program that fits one grain cannot
	// straddle" rule.
	at4k := make([]byte, 0, 0x1000)
	for len(at4k) < 0x0FFE {
		at4k = append(at4k, op16...)
	}
	at4k = append(at4k, op32...)
	if _, _, ok := instructionStraddle(at4k, 0, 16*1024); ok {
		t.Fatalf("instruction across the 4 KiB line flagged under a 16 KiB grain")
	}
	if _, size, ok := instructionStraddle(at4k, 0, 4*1024); !ok || size != 4 {
		t.Fatalf("instruction across the 4 KiB line not flagged under a 4 KiB grain")
	}

	at16k := make([]byte, 0, 0x4000)
	for len(at16k) < 0x3FFE {
		at16k = append(at16k, op16...)
	}
	at16k = append(at16k, op32...)
	if _, _, ok := instructionStraddle(at16k, 0, 16*1024); !ok {
		t.Fatalf("instruction across the 16 KiB line not flagged under a 16 KiB grain")
	}

	// A stream that ends mid-instruction stops instead of reading past the end.
	if _, _, ok := instructionStraddle(append(append([]byte{}, op16...), 0x37), 0, 4096); ok {
		t.Fatalf("truncated instruction flagged")
	}
}
