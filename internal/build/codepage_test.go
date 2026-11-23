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
	addr, size, ok := instructionStraddle(code, 0)
	if !ok || size != 4 || addr != codePageBytes-2 {
		t.Fatalf("32-bit instruction at page_end-2 not flagged: ok=%v size=%d addr=0x%x",
			ok, size, addr)
	}

	// The same stream with a 16-bit instruction in that slot is legal: it
	// ends exactly on the boundary.
	ok16 := append([]byte{}, code[:codePageBytes-2]...)
	ok16 = append(ok16, op16...)
	if addr, size, ok := instructionStraddle(ok16, 0); ok {
		t.Fatalf("16-bit instruction at page_end-2 flagged: size=%d addr=0x%x", size, addr)
	}

	// A 32-bit instruction that ends exactly on the boundary is fine.
	fine := append([]byte{}, code[:codePageBytes-4]...)
	fine = append(fine, op32...)
	if addr, size, ok := instructionStraddle(fine, 0); ok {
		t.Fatalf("instruction ending on the boundary flagged: size=%d addr=0x%x", size, addr)
	}

	// Alignment comes from the linked base: the same bytes are safe when the
	// image is placed two bytes lower.
	if addr, size, ok := instructionStraddle(code, 0); !ok {
		t.Fatalf("expected a straddle at base 0 (size %d addr 0x%x)", size, addr)
	}
	shifted := append(append([]byte{}, op16...), code...)
	if _, _, ok := instructionStraddle(shifted, 0); ok {
		t.Fatalf("shifted stream should not straddle")
	}

	// A stream that ends mid-instruction stops instead of reading past the end.
	if _, _, ok := instructionStraddle(append(append([]byte{}, op16...), 0x37), 0); ok {
		t.Fatalf("truncated instruction flagged")
	}
}
