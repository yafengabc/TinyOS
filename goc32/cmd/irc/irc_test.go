package main

import (
	"encoding/binary"
	"strings"
	"testing"
)

func TestConstReturnELF(t *testing.T) {
	img := &image{
		nir:    2,
		entry:  0,
		nfuncs: 1,
		code: []insn{
			{op: opConst, a: 7, d: 0},
			{op: opRet, a: 0},
		},
		funcs: []uint32{0},
	}
	g := &gen{img: img}
	text, err := g.gen()
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(text, "_start:") || !strings.Contains(text, "rir_f0:") {
		t.Fatalf("assembly is missing entry labels:\n%s", text)
	}
	elf, err := buildELF(text, "test.s")
	if err != nil {
		t.Fatal(err)
	}
	if len(elf) < 52 || string(elf[:4]) != "\x7fELF" {
		t.Fatal("not an ELF32 image")
	}
	if got := binary.LittleEndian.Uint16(elf[18:]); got != 243 {
		t.Fatalf("e_machine=%d, want EM_RISCV", got)
	}
	if got := binary.LittleEndian.Uint16(elf[16:]); got != 3 {
		t.Fatalf("e_type=%d, want ET_DYN", got)
	}
	if got := binary.LittleEndian.Uint32(elf[24:]); got != 0 {
		t.Fatalf("e_entry=%d, want 0", got)
	}
}

func TestBranchLabels(t *testing.T) {
	// main: r0 = 1; JZ r0 -> 4; print(r0); RET r0; r0 = 0
	img := &image{
		nir:    5,
		entry:  0,
		nfuncs: 1,
		code: []insn{
			{op: opConst, a: 1, d: 0},
			{op: opJZ, a: 0, b: 4},
			{op: opPrint, a: 0},
			{op: opRet, a: 0},
			{op: opConst, a: 0, d: 0},
		},
		funcs: []uint32{0},
	}
	g := &gen{img: img}
	text, err := g.gen()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := buildELF(text, "branch.s"); err != nil {
		t.Fatal(err)
	}
}

// Every opcode must survive emission and assembling, even if this image is
// not meant to execute.  This is the guard against a new RIR2 opcode silently
// reaching only rirvm.
func TestAllOpcodesAssemble(t *testing.T) {
	img := &image{
		dsize:   16,
		nfuncs:  1,
		strpool: []byte{'x', 0},
		arena:   make([]byte, 16),
	}
	put := func(op, a, b, c, d int32) int {
		img.code = append(img.code, insn{op: op, a: a, b: b, c: c, d: d})
		return len(img.code) - 1
	}

	put(opConst, 7, 0, 0, 0)
	put(opMov, 0, 0, 0, 1)
	put(opAdd, 0, 1, 0, 2)
	put(opSub, 0, 1, 0, 2)
	put(opMul, 0, 1, 0, 2)
	put(opDiv, 0, 1, 0, 2)
	put(opLT, 0, 1, 0, 2)
	put(opGT, 0, 1, 0, 2)
	put(opEQ, 0, 1, 0, 2)
	put(opNE, 0, 1, 0, 2)
	put(opLE, 0, 1, 0, 2)
	put(opGE, 0, 1, 0, 2)
	put(opMod, 0, 1, 0, 2)
	put(opInc, 0, 0, 0, 2)
	put(opDec, 0, 0, 0, 2)
	put(opAddI, 0, 1, 0, 2)
	put(opSubI, 0, 1, 0, 2)
	put(opMulI, 0, 2, 0, 2)
	put(opShlI, 0, 1, 0, 2)
	put(opShrI, 0, 1, 0, 2)

	put(opLoadG, 0, 0, 0, 0)
	put(opStoreG, 0, 0, 0, 0)
	put(opLoadA, 0, 0, 1, 0)
	put(opStoreA, 0, 0, 0, 1)
	put(opStoreA_I, 0, 0, 0, 1)
	put(opLoadB, 0, 0, 0, 0)
	put(opStoreB, 0, 0, 0, 0)
	put(opLoadD, 0, 0, 0, 0)
	put(opStoreD, 0, 0, 0, 0)
	put(opBounds, 0, 1, 0, 0)
	put(opLoadL, 0, 0, 0, 0)
	put(opStoreL, 0, 0, 0, 0)

	put(opArg, 0, 0, 0, 0)
	put(opCall, 0, 0, 0, 0)
	put(opPrint, 0, 0, 0, 0)
	put(opPrintV, 0, 0, 0, 0)
	put(opPrints, 0, 0, 0, 0)
	put(opPrintN, 0, 1, 0, 0)
	put(opPrintStr, 0, 0, 0, 0)
	put(opFMul, 0, 1, 0, 2)
	put(opFDiv, 0, 1, 0, 2)
	put(opPrintFix, 0, 0, 0, 0)
	put(opPrintFixV, 0, 0, 0, 0)
	put(opPrintFixN, 0, 2, 0, 0)
	put(opStrCpy, 0, 0, 0, 1)
	put(opStrCat, 0, 0, 0, 1)
	put(opStrLen, 0, 0, 0, 1)
	put(opStrCmp, 0, 0, 0, 1)
	put(opAtoi, 0, 0, 0, 1)
	put(opNative, 9, 0, -1, -1)
	put(opNativeR, 0, -1, -1, 0)
	put(opNop, 0, 0, 0, 0)

	// Branches target a real label: append the targets, then patch them to
	// the final RET.
	jmp := put(opJMP, 0, 0, 0, 0)
	jz := put(opJZ, 0, 0, 0, 0)
	fused := []int{
		put(opJLT, 0, 0, 0, 0), put(opJGE, 0, 0, 0, 0),
		put(opJGT, 0, 0, 0, 0), put(opJLE, 0, 0, 0, 0),
		put(opJEQ, 0, 0, 0, 0), put(opJNE, 0, 0, 0, 0),
		put(opJLT_I, 0, 0, 0, 0), put(opJGE_I, 0, 0, 0, 0),
		put(opJGT_I, 0, 0, 0, 0), put(opJLE_I, 0, 0, 0, 0),
		put(opJEQ_I, 0, 0, 0, 0), put(opJNE_I, 0, 0, 0, 0),
	}
	last := put(opRet, 0, 0, 0, 0)
	img.nir = len(img.code)
	img.funcs = []uint32{0}
	img.entry = 0

	target := int32(last)
	img.code[jmp].a = target
	img.code[jz].b = target
	for _, i := range fused {
		img.code[i].c = target
	}

	g := &gen{img: img}
	text, err := g.gen()
	if err != nil {
		t.Fatal(err)
	}
	if _, err := buildELF(text, "all.s"); err != nil {
		t.Fatal(err)
	}
}
