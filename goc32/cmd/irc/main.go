// irc: translate a RIR2 image into a TinyOS-loadable RV32 ELF (LZ4TOS).
//
// This is the native counterpart of rirvm.  RIR2 already carries physical
// register numbers, so the backend maps r0..r12 to callee-saved RV32
// registers and treats r13..r15 as per-instruction scratch.
package main

import (
	"bytes"
	"encoding/binary"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"goc32/internal/as"
	"goc32/internal/ld"
	"goc32/internal/lz4tos"
	"goc32/internal/obj"
)

const (
	opConst = iota
	opMov
	opAdd
	opSub
	opMul
	opDiv
	opLT
	opGT
	opEQ
	opJZ
	opJMP
	opRet
	opHalt
	opLoadG
	opStoreG
	opLoadA
	opStoreA
	opPrint
	opArg
	opCall
	opLoadL
	opStoreL
	opInc
	opDec
	opAddI
	opSubI
	opMulI
	opJLT
	opJGE
	opJGT
	opJLE
	opJEQ
	opJNE
	opJLT_I
	opJGE_I
	opJGT_I
	opJLE_I
	opJEQ_I
	opJNE_I
	opStoreA_I
	opNop
	opPrints
	opPrintN
	opPrintV
	opNative
	opNativeR
	opLoadD
	opStoreD
	opShlI
	opShrI
	opFMul
	opFDiv
	opPrintFix
	opPrintFixV
	opBounds
	opPrintFixN
	opMod
	opLoadB
	opStoreB
	opStrCpy
	opStrCat
	opStrLen
	opStrCmp
	opAtoi
	opPrintStr
	opNE
	opLE
	opGE
)

type insn struct {
	op, a, b, c, d int32
}

type image struct {
	nir     int
	gsize   int32
	entry   int32
	nfuncs  int32
	strsize int32
	dsize   int32
	code    []insn
	funcs   []uint32
	strpool []byte
	arena   []byte
}

func loadImage(path string) (*image, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if len(data) < 28 || string(data[:4]) != "RIR2" {
		return nil, fmt.Errorf("%s: not a RIR2 image", path)
	}
	field := func(off int) int32 { return int32(binary.LittleEndian.Uint32(data[off:])) }
	img := &image{
		nir:     int(field(4)),
		gsize:   field(8),
		entry:   field(12),
		nfuncs:  field(16),
		strsize: field(20),
		dsize:   field(24),
	}
	if img.nir <= 0 || img.nfuncs <= 0 || img.gsize < 0 ||
		img.strsize < 0 || img.dsize < 0 {
		return nil, fmt.Errorf("%s: bad header", path)
	}
	need := 28 + img.nir*20 + int(img.nfuncs)*4 + int(img.strsize) + int(img.dsize)
	if need > len(data) {
		return nil, fmt.Errorf("%s: truncated image", path)
	}
	off := 28
	img.code = make([]insn, img.nir)
	for i := range img.code {
		img.code[i] = insn{
			op: int32(binary.LittleEndian.Uint32(data[off:])),
			a:  int32(binary.LittleEndian.Uint32(data[off+4:])),
			b:  int32(binary.LittleEndian.Uint32(data[off+8:])),
			c:  int32(binary.LittleEndian.Uint32(data[off+12:])),
			d:  int32(binary.LittleEndian.Uint32(data[off+16:])),
		}
		off += 20
	}
	img.funcs = make([]uint32, img.nfuncs)
	for i := range img.funcs {
		img.funcs[i] = binary.LittleEndian.Uint32(data[off:])
		off += 4
	}
	img.strpool = data[off : off+int(img.strsize)]
	off += int(img.strsize)
	img.arena = data[off : off+int(img.dsize)]
	return img, nil
}

type funcRange struct {
	id, start, end int
}

type gen struct {
	img     *image
	b       strings.Builder
	maxArgc []int
	funcs   []funcRange
	mainID  int
	uid     int
}

func (g *gen) p(format string, args ...interface{}) {
	fmt.Fprintf(&g.b, format+"\n", args...)
}

func (g *gen) nextLabel() int {
	g.uid++
	return g.uid
}

func rreg(n int32) (string, error) {
	switch {
	case n >= 0 && n <= 11:
		return fmt.Sprintf("s%d", n), nil
	case n == 12:
		return "gp", nil
	case n >= 13 && n <= 15:
		return fmt.Sprintf("t%d", n-13), nil
	default:
		return "", fmt.Errorf("bad RIR register %d", n)
	}
}

func (g *gen) reg(n int32) string {
	r, err := rreg(n)
	if err != nil {
		panic(err)
	}
	return r
}

func (g *gen) gen() (string, error) {
	if err := g.analyze(); err != nil {
		return "", err
	}
	g.emitStart()
	g.emitProgram()
	g.emitHelpers()
	g.emitData()
	return g.b.String(), nil
}

func (g *gen) analyze() error {
	g.maxArgc = make([]int, len(g.img.funcs))
	g.funcs = make([]funcRange, len(g.img.funcs))
	for id, e := range g.img.funcs {
		g.funcs[id] = funcRange{id: id, start: int(e), end: g.img.nir}
	}
	sort.Slice(g.funcs, func(i, j int) bool { return g.funcs[i].start < g.funcs[j].start })
	for i := range g.funcs {
		if i+1 < len(g.funcs) {
			g.funcs[i].end = g.funcs[i+1].start
		}
		if g.funcs[i].start < 0 || g.funcs[i].end > g.img.nir ||
			g.funcs[i].start >= g.funcs[i].end {
			return fmt.Errorf("bad function range for id %d", g.funcs[i].id)
		}
	}
	g.mainID = -1
	for _, f := range g.funcs {
		if f.start == int(g.img.entry) {
			g.mainID = f.id
			break
		}
	}
	if g.mainID < 0 {
		return fmt.Errorf("entry pc %d is not a function start", g.img.entry)
	}
	for _, in := range g.img.code {
		if in.op == opCall {
			if in.a < 0 || int(in.a) >= len(g.maxArgc) {
				return fmt.Errorf("CALL to bad function id %d", in.a)
			}
			if int(in.c) > g.maxArgc[in.a] {
				g.maxArgc[in.a] = int(in.c)
			}
		}
	}
	return nil
}

func (g *gen) emitStart() {
	g.p(".section .text.entry,\"ax\"")
	g.p(".globl _start")
	g.p("_start:")
	g.p("call rir_f%d", g.mainID)
	g.p("li a7, 0")
	g.p("ecall")
	g.p(".Lhalt:")
	g.p("j .Lhalt")
}

func (g *gen) frameFor(f funcRange) (int, int) {
	maxSlot := -1
	for pc := f.start; pc < f.end; pc++ {
		in := g.img.code[pc]
		switch in.op {
		case opLoadL:
			if int(in.a) > maxSlot {
				maxSlot = int(in.a)
			}
		case opStoreL:
			if int(in.b) > maxSlot {
				maxSlot = int(in.b)
			}
		}
	}
	frame := 56 + 4*(maxSlot+1)
	if frame < 64 {
		frame = 64
	}
	return (frame + 15) &^ 15, maxSlot
}

func (g *gen) emitProgram() {
	g.p(".section .text")
	for _, f := range g.funcs {
		frame, _ := g.frameFor(f)
		g.p("rir_f%d:", f.id)
		g.p(".L%d:", f.start)
		g.p("addi sp, sp, -%d", frame)
		g.p("sw ra, 0(sp)")
		for i := 0; i < 12; i++ {
			g.p("sw s%d, %d(sp)", i, 4+4*i)
		}
		g.p("sw gp, 52(sp)")
		if g.maxArgc[f.id] > 0 {
			g.p("la t3, argbuf")
			n := g.maxArgc[f.id]
			if n > 13 {
				n = 13
			}
			for k := 0; k < n; k++ {
				g.p("lw %s, %d(t3)", g.reg(int32(k)), 4*k)
			}
		}
		for pc := f.start; pc < f.end; pc++ {
			if pc != f.start {
				g.p(".L%d:", pc)
			}
			g.emitInsn(f, pc, frame)
		}
		g.p(".Lepi%d:", f.id)
		g.p("lw ra, 0(sp)")
		for i := 0; i < 12; i++ {
			g.p("lw s%d, %d(sp)", i, 4+4*i)
		}
		g.p("lw gp, 52(sp)")
		g.p("addi sp, sp, %d", frame)
		g.p("ret")
	}
	g.p(".L%d:", g.img.nir)
	g.p("j .L%d", g.img.nir)
}

func (g *gen) emitInsn(f funcRange, pc int, frame int) {
	in := g.img.code[pc]
	ra := func(n int32) string { return g.reg(n) }
	switch in.op {
	case opConst:
		g.p("li %s, %d", ra(in.d), in.a)
	case opMov:
		g.p("mv %s, %s", ra(in.d), ra(in.a))
	case opAdd:
		g.p("add %s, %s, %s", ra(in.d), ra(in.a), ra(in.b))
	case opSub:
		g.p("sub %s, %s, %s", ra(in.d), ra(in.a), ra(in.b))
	case opMul:
		g.p("mul %s, %s, %s", ra(in.d), ra(in.a), ra(in.b))
	case opDiv:
		g.p("div %s, %s, %s", ra(in.d), ra(in.a), ra(in.b))
	case opMod:
		l := g.nextLabel()
		g.p("bnez %s, .Lmod%d", ra(in.b), l)
		g.p("li %s, 0", ra(in.d))
		g.p("j .Lmode%d", l)
		g.p(".Lmod%d:", l)
		g.p("rem %s, %s, %s", ra(in.d), ra(in.a), ra(in.b))
		g.p(".Lmode%d:", l)
	case opLT:
		g.p("slt %s, %s, %s", ra(in.d), ra(in.a), ra(in.b))
	case opGT:
		g.p("slt %s, %s, %s", ra(in.d), ra(in.b), ra(in.a))
	case opEQ:
		g.p("xor %s, %s, %s", ra(in.d), ra(in.a), ra(in.b))
		g.p("seqz %s, %s", ra(in.d), ra(in.d))
	case opNE:
		g.p("xor %s, %s, %s", ra(in.d), ra(in.a), ra(in.b))
		g.p("snez %s, %s", ra(in.d), ra(in.d))
	case opLE:
		g.p("slt %s, %s, %s", ra(in.d), ra(in.b), ra(in.a))
		g.p("xori %s, %s, 1", ra(in.d), ra(in.d))
	case opGE:
		g.p("slt %s, %s, %s", ra(in.d), ra(in.a), ra(in.b))
		g.p("xori %s, %s, 1", ra(in.d), ra(in.d))
	case opJZ:
		l := g.nextLabel()
		g.p("bnez %s, .Lsk%d", ra(in.a), l)
		g.p("j .L%d", in.b)
		g.p(".Lsk%d:", l)
	case opJMP:
		g.p("j .L%d", in.a)
	case opRet:
		g.p("mv a0, %s", ra(in.a))
		g.p("j .Lepi%d", f.id)
	case opHalt:
		g.p("li a0, 0")
		g.p("li a7, 0")
		g.p("ecall")
	case opLoadG:
		g.p("la t3, g")
		g.p("lw %s, %d(t3)", ra(in.d), in.a*4)
	case opStoreG:
		g.p("la t3, g")
		g.p("sw %s, %d(t3)", ra(in.b), in.a*4)
	case opLoadA:
		g.loadArrayTail(in.a, ra(in.b), ra(in.d), in.c, true)
	case opStoreA:
		g.loadArrayTail(in.a, ra(in.b), "", in.d, false)
		g.p("sw %s, 0(t4)", ra(in.c))
	case opStoreA_I:
		g.loadArrayTail(in.a, ra(in.b), "", in.d, false)
		g.p("li t6, %d", in.c)
		g.p("sw t6, 0(t4)")
	case opPrint:
		g.p("mv a0, %s", ra(in.a))
		g.p("call _rir_print_int")
	case opPrintV:
		g.p("mv a0, %s", ra(in.a))
		g.p("call _rir_print_int_n")
	case opPrints:
		g.printString(int(in.a), stringLen(g.img.strpool, int(in.a)))
	case opPrintN:
		g.printString(int(in.a), int(in.b))
	case opPrintStr:
		g.p("mv a0, %s", ra(in.a))
		g.p("call _rir_print_arena")
	case opArg:
		g.p("la t3, argbuf")
		g.p("sw %s, %d(t3)", ra(in.b), in.a*4)
	case opCall:
		g.p("call rir_f%d", in.a)
		g.p("mv %s, a0", ra(in.b))
	case opLoadL:
		g.p("lw %s, %d(sp)", ra(in.d), 56+4*in.a)
	case opStoreL:
		g.p("sw %s, %d(sp)", ra(in.a), 56+4*in.b)
	case opInc:
		g.p("addi %s, %s, 1", ra(in.d), ra(in.a))
	case opDec:
		g.p("addi %s, %s, -1", ra(in.d), ra(in.a))
	case opAddI:
		g.p("li t3, %d", in.b)
		g.p("add %s, %s, t3", ra(in.d), ra(in.a))
	case opSubI:
		g.p("li t3, %d", in.b)
		g.p("sub %s, %s, t3", ra(in.d), ra(in.a))
	case opMulI:
		g.p("li t3, %d", in.b)
		g.p("mul %s, %s, t3", ra(in.d), ra(in.a))
	case opJLT:
		g.condJump("bge", ra(in.a), ra(in.b), int(in.c))
	case opJGE:
		g.condJump("blt", ra(in.a), ra(in.b), int(in.c))
	case opJGT:
		g.condJump("bge", ra(in.b), ra(in.a), int(in.c))
	case opJLE:
		g.condJump("blt", ra(in.b), ra(in.a), int(in.c))
	case opJEQ:
		g.condJump("bne", ra(in.a), ra(in.b), int(in.c))
	case opJNE:
		g.condJump("beq", ra(in.a), ra(in.b), int(in.c))
	case opJLT_I:
		g.p("li t3, %d", in.b)
		g.condJump("bge", ra(in.a), "t3", int(in.c))
	case opJGE_I:
		g.p("li t3, %d", in.b)
		g.condJump("blt", ra(in.a), "t3", int(in.c))
	case opJGT_I:
		g.p("li t3, %d", in.b)
		g.condJump("bge", "t3", ra(in.a), int(in.c))
	case opJLE_I:
		g.p("li t3, %d", in.b)
		g.condJump("blt", "t3", ra(in.a), int(in.c))
	case opJEQ_I:
		g.p("li t3, %d", in.b)
		g.condJump("bne", ra(in.a), "t3", int(in.c))
	case opJNE_I:
		g.p("li t3, %d", in.b)
		g.condJump("beq", ra(in.a), "t3", int(in.c))
	case opNop:
		g.p("nop")
	case opLoadD:
		g.p("slli t3, %s, 2", ra(in.a))
		if in.b != 0 {
			g.p("addi t3, t3, %d", in.b*4)
		}
		g.p("la t4, g")
		g.p("add t3, t4, t3")
		g.p("lw %s, 0(t3)", ra(in.d))
	case opStoreD:
		g.p("slli t3, %s, 2", ra(in.a))
		if in.b != 0 {
			g.p("addi t3, t3, %d", in.b*4)
		}
		g.p("la t4, g")
		g.p("add t3, t4, t3")
		g.p("sw %s, 0(t3)", ra(in.c))
	case opShlI:
		g.p("slli %s, %s, %d", ra(in.d), ra(in.a), in.b)
	case opShrI:
		g.p("srai %s, %s, %d", ra(in.d), ra(in.a), in.b)
	case opBounds:
		g.bounds(ra(in.a), in.b)
	case opLoadB:
		g.loadByte(in.a, ra(in.b), ra(in.d), true)
	case opStoreB:
		g.loadByte(in.a, ra(in.b), "", false)
		g.p("sb %s, 0(t4)", ra(in.c))
	case opNative:
		g.emitNative(in.a, in.b, in.c, in.d, "")
	case opNativeR:
		g.emitNative(in.a, in.b, in.c, -1, ra(in.d))
	case opStrCpy:
		g.p("mv a0, %s", ra(in.a))
		g.p("mv a1, %s", ra(in.b))
		g.p("mv a2, %s", ra(in.c))
		g.p("call _rir_strcpy")
		g.p("mv %s, a0", ra(in.d))
	case opStrCat:
		g.p("mv a0, %s", ra(in.a))
		g.p("mv a1, %s", ra(in.b))
		g.p("mv a2, %s", ra(in.c))
		g.p("call _rir_strcat")
		g.p("mv %s, a0", ra(in.d))
	case opStrLen:
		g.p("mv a0, %s", ra(in.a))
		g.p("call _rir_strlen")
		g.p("mv %s, a0", ra(in.d))
	case opStrCmp:
		g.p("mv a0, %s", ra(in.a))
		g.p("mv a1, %s", ra(in.b))
		g.p("call _rir_strcmp")
		g.p("mv %s, a0", ra(in.d))
	case opAtoi:
		g.p("mv a0, %s", ra(in.a))
		g.p("call _rir_atoi")
		g.p("mv %s, a0", ra(in.d))
	case opFMul:
		g.p("mv a0, %s", ra(in.a))
		g.p("mv a1, %s", ra(in.b))
		g.p("call _rir_fmul")
		g.p("mv %s, a0", ra(in.d))
	case opFDiv:
		g.p("mv a0, %s", ra(in.a))
		g.p("mv a1, %s", ra(in.b))
		g.p("call _rir_fdiv")
		g.p("mv %s, a0", ra(in.d))
	case opPrintFix:
		g.p("mv a0, %s", ra(in.a))
		g.p("call _rir_print_fix")
	case opPrintFixV:
		g.p("mv a0, %s", ra(in.a))
		g.p("call _rir_print_fix_nl0")
	case opPrintFixN:
		g.p("mv a0, %s", ra(in.a))
		g.p("li a1, %d", in.b)
		g.p("call _rir_print_fix_n")
	default:
		panic(fmt.Errorf("unknown opcode %d at pc %d", in.op, pc))
	}
}

func (g *gen) condJump(inv, rs1, rs2 string, target int) {
	l := g.nextLabel()
	g.p("%s %s, %s, .Lsk%d", inv, rs1, rs2, l)
	g.p("j .L%d", target)
	g.p(".Lsk%d:", l)
}

func (g *gen) bounds(idx string, n int32) {
	l := g.nextLabel()
	g.p("li t3, %d", n)
	g.p("bltu %s, t3, .Lbo%d", idx, l)
	g.p("call _rir_bounds")
	g.p(".Lbo%d:", l)
}

// loadArrayTail checks idx<bounds and leaves the element address in t4.
func (g *gen) loadArrayTail(off int32, idx, dst string, n int32, load bool) {
	l := g.nextLabel()
	g.p("li t3, %d", n)
	g.p("bltu %s, t3, .Lao%d", idx, l)
	g.p("call _rir_bounds")
	g.p(".Lao%d:", l)
	g.p("la t4, g")
	if off != 0 {
		g.p("li t5, %d", off*4)
		g.p("add t4, t4, t5")
	}
	g.p("slli t3, %s, 2", idx)
	g.p("add t4, t4, t3")
	if load {
		g.p("lw %s, 0(t4)", dst)
	}
}

func (g *gen) loadByte(base int32, idx, dst string, load bool) {
	l := g.nextLabel()
	f := g.nextLabel()
	g.p("blt %s, zero, .Lbf%d", idx, f)
	g.p("li t4, %d", base)
	g.p("add t4, t4, %s", idx)
	g.p("li t3, %d", g.img.dsize)
	g.p("bltu t4, t3, .Lbo%d", l)
	g.p(".Lbf%d:", f)
	g.p("call _rir_bounds")
	g.p(".Lbo%d:", l)
	g.p("la t3, arena")
	g.p("add t4, t3, t4")
	if load {
		g.p("lbu %s, 0(t4)", dst)
	}
}

func (g *gen) printString(off, n int) {
	g.p("la a1, strpool")
	if off != 0 {
		g.p("li t3, %d", off)
		g.p("add a1, a1, t3")
	}
	g.p("li a2, %d", n)
	g.p("li a0, 1")
	g.p("li a7, 1")
	g.p("ecall")
}

func stringLen(pool []byte, off int) int {
	if off < 0 || off >= len(pool) {
		return 0
	}
	if i := bytes.IndexByte(pool[off:], 0); i >= 0 {
		return i
	}
	return len(pool) - off
}

func (g *gen) emitNative(id, a0, a1, a2 int32, dst string) {
	arg := func(n int32) string {
		if n < 0 {
			return "zero"
		}
		return g.reg(n)
	}
	store := func() {
		if dst != "" {
			g.p("mv %s, a0", dst)
		}
	}
	switch id {
	case 0: // gpio_count
		g.p("li a0, 0")
		g.p("li a1, 0")
		g.p("li a2, 0")
		g.p("li a7, 26")
		g.p("ecall")
		store()
	case 1: // gpio_mode
		g.p("li a0, 1")
		g.p("mv a1, %s", arg(a0))
		g.p("mv a2, %s", arg(a1))
		g.p("li a7, 26")
		g.p("ecall")
	case 2: // gpio_write
		g.p("li a0, 2")
		g.p("mv a1, %s", arg(a0))
		g.p("mv a2, %s", arg(a1))
		g.p("li a7, 26")
		g.p("ecall")
	case 3: // gpio_read
		g.p("li a0, 3")
		g.p("mv a1, %s", arg(a0))
		g.p("li a2, 0")
		g.p("li a7, 26")
		g.p("ecall")
		store()
	case 4: // adc_count
		g.p("li a0, 0")
		g.p("li a1, 0")
		g.p("li a7, 27")
		g.p("ecall")
		store()
	case 5: // adc_read
		g.p("li a0, 1")
		g.p("mv a1, %s", arg(a0))
		g.p("li a7, 27")
		g.p("ecall")
		store()
	case 6: // pwm_count
		g.p("li a0, 0")
		g.p("li a1, 0")
		g.p("li a7, 40")
		g.p("ecall")
		store()
	case 7: // pwm_set
		g.p("la t3, pwmreq")
		g.p("mv t4, %s", arg(a0))
		g.p("sw t4, 0(t3)")
		g.p("mv t4, %s", arg(a1))
		g.p("sw t4, 4(t3)")
		g.p("mv t4, %s", arg(a2))
		g.p("sw t4, 8(t3)")
		g.p("li a0, 1")
		g.p("la a1, pwmreq")
		g.p("li a2, 0")
		g.p("li a7, 40")
		g.p("ecall")
	case 8: // pwm_off
		g.p("li a0, 2")
		g.p("mv a1, %s", arg(a0))
		g.p("li a2, 0")
		g.p("li a7, 40")
		g.p("ecall")
	case 9: // sleep
		g.p("mv a0, %s", arg(a0))
		g.p("li a7, 8")
		g.p("ecall")
	case 10: // uptime
		g.p("li a7, 9")
		g.p("ecall")
		store()
	default:
		panic(fmt.Errorf("unsupported native id %d", id))
	}
	_ = a1
	_ = a2
}

func (g *gen) emitHelpers() {
	g.p(".section .text")
	g.p("_rir_bounds:")
	g.p("la a1, .Lbounds_msg")
	g.p("li a2, 7")
	g.p("li a0, 1")
	g.p("li a7, 1")
	g.p("ecall")
	g.p("li a0, 1")
	g.p("li a7, 0")
	g.p("ecall")

	g.p("_rir_print_int:")
	g.p("li a1, 1")
	g.p("j _rir_print_int_common")
	g.p("_rir_print_int_n:")
	g.p("li a1, 0")
	g.p("_rir_print_int_common:")
	g.p("addi sp, sp, -32")
	g.p("sw ra, 28(sp)")
	g.p("sw a1, 24(sp)")
	g.p("mv t0, a0")
	g.p("addi t1, sp, 20")
	g.p("li t2, 0")
	g.p("bge t0, zero, .Lpi_pos")
	g.p("li t2, 1")
	g.p("sub t0, zero, t0")
	g.p(".Lpi_pos:")
	g.p("li t3, 10")
	g.p("mv t4, t1")
	g.p(".Lpi_digit:")
	g.p("remu t5, t0, t3")
	g.p("addi t5, t5, 48")
	g.p("addi t4, t4, -1")
	g.p("sb t5, 0(t4)")
	g.p("divu t0, t0, t3")
	g.p("bnez t0, .Lpi_digit")
	g.p("beqz t2, .Lpi_sign")
	g.p("addi t4, t4, -1")
	g.p("li t5, 45")
	g.p("sb t5, 0(t4)")
	g.p(".Lpi_sign:")
	g.p("lw t6, 24(sp)")
	g.p("beqz t6, .Lpi_nowrap")
	g.p("li t5, 10")
	g.p("sb t5, 0(t1)")
	g.p("addi t1, t1, 1")
	g.p(".Lpi_nowrap:")
	g.p("sub a2, t1, t4")
	g.p("mv a1, t4")
	g.p("li a0, 1")
	g.p("li a7, 1")
	g.p("ecall")
	g.p("lw ra, 28(sp)")
	g.p("addi sp, sp, 32")
	g.p("ret")

	g.p("_rir_fmul:")
	g.p("xor t0, a0, a1")
	g.p("srai t0, t0, 31")
	g.p("srai t1, a0, 31")
	g.p("xor t2, a0, t1")
	g.p("sub t2, t2, t1")
	g.p("srai t1, a1, 31")
	g.p("xor t3, a1, t1")
	g.p("sub t3, t3, t1")
	g.p("slli t4, t2, 16")
	g.p("srli t4, t4, 16")
	g.p("srli t5, t2, 16")
	g.p("slli t6, t3, 16")
	g.p("srli t6, t6, 16")
	g.p("srli a2, t3, 16")
	g.p("mul a3, t4, t6")
	g.p("mul a4, t4, a2")
	g.p("mul a5, t5, t6")
	g.p("mul a6, t5, a2")
	g.p("srli a7, a3, 16")
	g.p("slli t4, a4, 16")
	g.p("srli t4, t4, 16")
	g.p("add a7, a7, t4")
	g.p("slli t4, a5, 16")
	g.p("srli t4, t4, 16")
	g.p("add a7, a7, t4")
	g.p("srli t5, a4, 16")
	g.p("add a6, a6, t5")
	g.p("srli t5, a5, 16")
	g.p("add a6, a6, t5")
	g.p("srli t5, a7, 16")
	g.p("add a6, a6, t5")
	g.p("slli a6, a6, 16")
	g.p("slli a7, a7, 16")
	g.p("srli a7, a7, 16")
	g.p("or a0, a6, a7")
	g.p("xor a0, a0, t0")
	g.p("sub a0, a0, t0")
	g.p("ret")

	g.p("_rir_fdiv:")
	g.p("bnez a1, .Lfd_nz")
	g.p("blt a0, zero, .Lfd_negmax")
	g.p("li a0, 2147483647")
	g.p("ret")
	g.p(".Lfd_negmax:")
	g.p("li a0, -2147483648")
	g.p("ret")
	g.p(".Lfd_nz:")
	g.p("xor t0, a0, a1")
	g.p("srai t0, t0, 31")
	g.p("srai t1, a0, 31")
	g.p("xor t2, a0, t1")
	g.p("sub t2, t2, t1")
	g.p("srai t1, a1, 31")
	g.p("xor t3, a1, t1")
	g.p("sub t3, t3, t1")
	g.p("divu t4, t2, t3")
	g.p("remu t5, t2, t3")
	g.p("li t6, 32767")
	g.p("blt t6, t4, .Lfd_clamp")
	g.p("li a2, 0")
	g.p("li a3, 0")
	g.p(".Lfd_loop:")
	g.p("li a4, 16")
	g.p("bge a3, a4, .Lfd_done")
	g.p("srli a5, t5, 31")
	g.p("slli t5, t5, 1")
	g.p("bnez a5, .Lfd_sub")
	g.p("bltu t5, t3, .Lfd_nosub")
	g.p(".Lfd_sub:")
	g.p("sub t5, t5, t3")
	g.p("li a4, 15")
	g.p("sub a4, a4, a3")
	g.p("li a5, 1")
	g.p("sll a5, a5, a4")
	g.p("or a2, a2, a5")
	g.p(".Lfd_nosub:")
	g.p("addi a3, a3, 1")
	g.p("j .Lfd_loop")
	g.p(".Lfd_done:")
	g.p("slli t4, t4, 16")
	g.p("or a0, t4, a2")
	g.p("xor a0, a0, t0")
	g.p("sub a0, a0, t0")
	g.p("ret")
	g.p(".Lfd_clamp:")
	g.p("beqz t0, .Lfd_posmax")
	g.p("li a0, -2147483648")
	g.p("ret")
	g.p(".Lfd_posmax:")
	g.p("li a0, 2147483647")
	g.p("ret")

	g.p("_rir_print_fix:")
	g.p("li a1, 1")
	g.p("j _rir_print_fix_common")
	g.p("_rir_print_fix_nl0:")
	g.p("li a1, 0")
	g.p("_rir_print_fix_common:")
	g.p("addi sp, sp, -32")
	g.p("sw ra, 28(sp)")
	g.p("sw a1, 4(sp)")
	g.p("sw a0, 0(sp)")
	g.p("lw t0, 0(sp)")
	g.p("bge t0, zero, .Lpf_pos")
	g.p("li t0, 45")
	g.p("sb t0, 16(sp)")
	g.p("li a0, 1")
	g.p("addi a1, sp, 16")
	g.p("li a2, 1")
	g.p("li a7, 1")
	g.p("ecall")
	g.p("lw t0, 0(sp)")
	g.p("sub t0, zero, t0")
	g.p("sw t0, 0(sp)")
	g.p(".Lpf_pos:")
	g.p("lw t0, 0(sp)")
	g.p("srli a0, t0, 16")
	g.p("call _rir_print_int_n")
	g.p("li t0, 46")
	g.p("sb t0, 16(sp)")
	g.p("li a0, 1")
	g.p("addi a1, sp, 16")
	g.p("li a2, 1")
	g.p("li a7, 1")
	g.p("ecall")
	g.p("lw t0, 0(sp)")
	g.p("slli t0, t0, 16")
	g.p("srli t0, t0, 16")
	g.p("li t1, 10000")
	g.p("mul t0, t0, t1")
	g.p("li t1, 32768")
	g.p("add t0, t0, t1")
	g.p("srli t0, t0, 16")
	g.p("sw t0, 8(sp)")
	g.p("li t1, 10")
	g.p("li t3, 3")
	g.p(".Lpf_dig:")
	g.p("remu t4, t0, t1")
	g.p("addi t4, t4, 48")
	g.p("addi t5, sp, 12")
	g.p("add t5, t5, t3")
	g.p("sb t4, 0(t5)")
	g.p("divu t0, t0, t1")
	g.p("addi t3, t3, -1")
	g.p("bge t3, zero, .Lpf_dig")
	g.p("li t3, 4")
	g.p(".Lpf_trim:")
	g.p("li t4, 1")
	g.p("bge t4, t3, .Lpf_emit")
	g.p("addi t5, sp, 12")
	g.p("add t5, t5, t3")
	g.p("addi t5, t5, -1")
	g.p("lbu t4, 0(t5)")
	g.p("li t6, 48")
	g.p("bne t4, t6, .Lpf_emit")
	g.p("addi t3, t3, -1")
	g.p("j .Lpf_trim")
	g.p(".Lpf_emit:")
	g.p("addi a1, sp, 12")
	g.p("mv a2, t3")
	g.p("li a0, 1")
	g.p("li a7, 1")
	g.p("ecall")
	g.p("lw t4, 4(sp)")
	g.p("beqz t4, .Lpf_done")
	g.p("li t0, 10")
	g.p("sb t0, 16(sp)")
	g.p("li a0, 1")
	g.p("addi a1, sp, 16")
	g.p("li a2, 1")
	g.p("li a7, 1")
	g.p("ecall")
	g.p(".Lpf_done:")
	g.p("lw ra, 28(sp)")
	g.p("addi sp, sp, 32")
	g.p("ret")

	g.p("_rir_print_fix_n:")
	g.p("addi sp, sp, -48")
	g.p("sw ra, 44(sp)")
	g.p("sw a0, 0(sp)")
	g.p("sw a1, 4(sp)")
	g.p("blt a1, zero, .Lpn_d0")
	g.p("li t0, 4")
	g.p("blt t0, a1, .Lpn_d4")
	g.p("j .Lpn_dok")
	g.p(".Lpn_d0:")
	g.p("li a1, 0")
	g.p("sw a1, 4(sp)")
	g.p("j .Lpn_dok")
	g.p(".Lpn_d4:")
	g.p("li a1, 4")
	g.p("sw a1, 4(sp)")
	g.p(".Lpn_dok:")
	g.p("lw t0, 0(sp)")
	g.p("li t5, 0")
	g.p("bge t0, zero, .Lpn_pos")
	g.p("li t5, 1")
	g.p("sub t0, zero, t0")
	g.p("sw t0, 0(sp)")
	g.p(".Lpn_pos:")
	g.p("sw t5, 28(sp)")
	g.p("lw t0, 0(sp)")
	g.p("slli t0, t0, 16")
	g.p("srli t0, t0, 16")
	g.p("lw a1, 4(sp)")
	g.p("la t1, .Lp10")
	g.p("slli t2, a1, 2")
	g.p("add t1, t1, t2")
	g.p("lw t2, 0(t1)")
	g.p("mul t0, t0, t2")
	g.p("li t3, 32768")
	g.p("add t0, t0, t3")
	g.p("srli t0, t0, 16")
	g.p("lw t4, 0(sp)")
	g.p("srli t4, t4, 16")
	g.p("blt t0, t2, .Lpn_nocarry")
	g.p("sub t0, t0, t2")
	g.p("addi t4, t4, 1")
	g.p(".Lpn_nocarry:")
	g.p("sw t4, 8(sp)")
	g.p("sw t0, 12(sp)")
	g.p("lw t5, 28(sp)")
	g.p("beqz t5, .Lpn_nosign")
	g.p("li t0, 45")
	g.p("sb t0, 24(sp)")
	g.p("li a0, 1")
	g.p("addi a1, sp, 24")
	g.p("li a2, 1")
	g.p("li a7, 1")
	g.p("ecall")
	g.p(".Lpn_nosign:")
	g.p("lw a0, 8(sp)")
	g.p("call _rir_print_int_n")
	g.p("lw t3, 4(sp)")
	g.p("beqz t3, .Lpn_done")
	g.p("li t0, 46")
	g.p("sb t0, 24(sp)")
	g.p("li a0, 1")
	g.p("addi a1, sp, 24")
	g.p("li a2, 1")
	g.p("li a7, 1")
	g.p("ecall")
	g.p("lw t3, 4(sp)")
	g.p("addi t3, t3, -1")
	g.p("lw t0, 12(sp)")
	g.p("li t1, 10")
	g.p(".Lpn_digits:")
	g.p("remu t2, t0, t1")
	g.p("addi t2, t2, 48")
	g.p("addi t4, sp, 16")
	g.p("add t4, t4, t3")
	g.p("sb t2, 0(t4)")
	g.p("divu t0, t0, t1")
	g.p("addi t3, t3, -1")
	g.p("bge t3, zero, .Lpn_digits")
	g.p("lw a2, 4(sp)")
	g.p("addi a1, sp, 16")
	g.p("li a0, 1")
	g.p("li a7, 1")
	g.p("ecall")
	g.p(".Lpn_done:")
	g.p("lw ra, 44(sp)")
	g.p("addi sp, sp, 48")
	g.p("ret")

	g.p("_rir_strlen:")
	g.p("li t0, 0")
	g.p("blt a0, zero, .Lsl_ret")
	g.p("la t1, arena")
	g.p("add t1, t1, a0")
	g.p("li t2, %d", g.img.dsize)
	g.p("sub t2, t2, a0")
	g.p(".Lsl_loop:")
	g.p("bge t0, t2, .Lsl_ret")
	g.p("lbu t3, 0(t1)")
	g.p("beqz t3, .Lsl_ret")
	g.p("addi t1, t1, 1")
	g.p("addi t0, t0, 1")
	g.p("j .Lsl_loop")
	g.p(".Lsl_ret:")
	g.p("mv a0, t0")
	g.p("ret")

	g.p("_rir_strcmp:")
	g.p("li t3, %d", g.img.dsize)
	g.p("la t1, arena")
	g.p("blt a0, zero, .Lsc_a_empty")
	g.p("bge a0, t3, .Lsc_a_empty")
	g.p("add t1, t1, a0")
	g.p("sub t4, t3, a0")
	g.p("j .Lsc_a_ok")
	g.p(".Lsc_a_empty:")
	g.p("la t1, .Lempty")
	g.p("li t4, 1")
	g.p(".Lsc_a_ok:")
	g.p("la t2, arena")
	g.p("blt a1, zero, .Lsc_b_empty")
	g.p("bge a1, t3, .Lsc_b_empty")
	g.p("add t2, t2, a1")
	g.p("sub t5, t3, a1")
	g.p("j .Lsc_b_ok")
	g.p(".Lsc_b_empty:")
	g.p("la t2, .Lempty")
	g.p("li t5, 1")
	g.p(".Lsc_b_ok:")
	g.p("blt t4, t5, .Lsc_min")
	g.p("mv t3, t5")
	g.p("j .Lsc_min_done")
	g.p(".Lsc_min:")
	g.p("mv t3, t4")
	g.p(".Lsc_min_done:")
	g.p("li t0, 0")
	g.p(".Lsc_loop:")
	g.p("bge t0, t3, .Lsc_eq")
	g.p("lbu t4, 0(t1)")
	g.p("lbu t5, 0(t2)")
	g.p("bne t4, t5, .Lsc_diff")
	g.p("beqz t4, .Lsc_eq")
	g.p("addi t1, t1, 1")
	g.p("addi t2, t2, 1")
	g.p("addi t0, t0, 1")
	g.p("j .Lsc_loop")
	g.p(".Lsc_diff:")
	g.p("bltu t4, t5, .Lsc_less")
	g.p("li a0, 1")
	g.p("ret")
	g.p(".Lsc_less:")
	g.p("li a0, -1")
	g.p("ret")
	g.p(".Lsc_eq:")
	g.p("li a0, 0")
	g.p("ret")

	g.p("_rir_strcpy:")
	g.p("addi sp, sp, -32")
	g.p("sw ra, 28(sp)")
	g.p("sw a0, 0(sp)")
	g.p("sw a1, 4(sp)")
	g.p("sw a2, 8(sp)")
	g.p("mv a0, a1")
	g.p("call _rir_strlen")
	g.p("mv a3, a0")
	g.p("lw a0, 0(sp)")
	g.p("lw a1, 4(sp)")
	g.p("lw a2, 8(sp)")
	g.p("blt a0, zero, .Lscp_ret")
	g.p("li t0, %d", g.img.dsize)
	g.p("bge a0, t0, .Lscp_ret")
	g.p("bge zero, a2, .Lscp_ret")
	g.p("addi t4, a2, -1")
	g.p("blt t4, a3, .Lscp_n")
	g.p("mv t4, a3")
	g.p(".Lscp_n:")
	g.p("la t1, arena")
	g.p("blt a1, zero, .Lscp_zero")
	g.p("add t1, t1, a1")
	g.p("j .Lscp_src")
	g.p(".Lscp_zero:")
	g.p("li t4, 0")
	g.p(".Lscp_src:")
	g.p("la t2, arena")
	g.p("add t2, t2, a0")
	g.p("la t6, arena")
	g.p("li t5, %d", g.img.dsize)
	g.p("add t6, t6, t5")
	g.p("li t0, 0")
	g.p(".Lscp_loop:")
	g.p("bge t0, t4, .Lscp_term")
	g.p("bgeu t2, t6, .Lscp_ret")
	g.p("lbu t5, 0(t1)")
	g.p("sb t5, 0(t2)")
	g.p("addi t1, t1, 1")
	g.p("addi t2, t2, 1")
	g.p("addi t0, t0, 1")
	g.p("j .Lscp_loop")
	g.p(".Lscp_term:")
	g.p("bgeu t2, t6, .Lscp_ret")
	g.p("sb zero, 0(t2)")
	g.p(".Lscp_ret:")
	g.p("mv a0, a3")
	g.p("lw ra, 28(sp)")
	g.p("addi sp, sp, 32")
	g.p("ret")

	g.p("_rir_strcat:")
	g.p("addi sp, sp, -48")
	g.p("sw ra, 44(sp)")
	g.p("sw a0, 0(sp)")
	g.p("sw a1, 4(sp)")
	g.p("sw a2, 8(sp)")
	g.p("call _rir_strlen")
	g.p("mv a3, a0")
	g.p("lw a1, 4(sp)")
	g.p("mv a0, a1")
	g.p("call _rir_strlen")
	g.p("mv a4, a0")
	g.p("lw a0, 0(sp)")
	g.p("lw a1, 4(sp)")
	g.p("lw a2, 8(sp)")
	g.p("add a5, a3, a4")
	g.p("blt a0, zero, .Lsca_ret")
	g.p("li t0, %d", g.img.dsize)
	g.p("bge a0, t0, .Lsca_ret")
	g.p("addi t4, a3, 1")
	g.p("bge t4, a2, .Lsca_ret")
	g.p("addi t4, a2, -1")
	g.p("sub t4, t4, a3")
	g.p("blt t4, a4, .Lsca_n")
	g.p("mv t4, a4")
	g.p(".Lsca_n:")
	g.p("la t1, arena")
	g.p("blt a1, zero, .Lsca_zero")
	g.p("add t1, t1, a1")
	g.p("j .Lsca_src")
	g.p(".Lsca_zero:")
	g.p("li t4, 0")
	g.p(".Lsca_src:")
	g.p("la t2, arena")
	g.p("add t2, t2, a0")
	g.p("add t2, t2, a3")
	g.p("la t6, arena")
	g.p("li t5, %d", g.img.dsize)
	g.p("add t6, t6, t5")
	g.p("li t0, 0")
	g.p(".Lsca_loop:")
	g.p("bge t0, t4, .Lsca_term")
	g.p("bgeu t2, t6, .Lsca_ret")
	g.p("lbu t5, 0(t1)")
	g.p("sb t5, 0(t2)")
	g.p("addi t1, t1, 1")
	g.p("addi t2, t2, 1")
	g.p("addi t0, t0, 1")
	g.p("j .Lsca_loop")
	g.p(".Lsca_term:")
	g.p("bgeu t2, t6, .Lsca_ret")
	g.p("sb zero, 0(t2)")
	g.p(".Lsca_ret:")
	g.p("mv a0, a5")
	g.p("lw ra, 44(sp)")
	g.p("addi sp, sp, 48")
	g.p("ret")

	g.p("_rir_atoi:")
	g.p("blt a0, zero, .Latoi_zero")
	g.p("la t1, arena")
	g.p("add t1, t1, a0")
	g.p("li t2, %d", g.img.dsize)
	g.p("sub t2, t2, a0")
	g.p("li t0, 0")
	g.p("li t6, 0")
	g.p(".Latoi_skip:")
	g.p("bge t0, t2, .Latoi_zero")
	g.p("lbu t3, 0(t1)")
	g.p("li t4, 32")
	g.p("beq t3, t4, .Latoi_skip1")
	g.p("li t4, 9")
	g.p("bne t3, t4, .Latoi_sign")
	g.p(".Latoi_skip1:")
	g.p("addi t1, t1, 1")
	g.p("addi t0, t0, 1")
	g.p("j .Latoi_skip")
	g.p(".Latoi_sign:")
	g.p("li t4, 45")
	g.p("bne t3, t4, .Latoi_plus")
	g.p("li t6, 1")
	g.p("addi t1, t1, 1")
	g.p("addi t0, t0, 1")
	g.p("j .Latoi_digits")
	g.p(".Latoi_plus:")
	g.p("li t4, 43")
	g.p("bne t3, t4, .Latoi_digits")
	g.p("addi t1, t1, 1")
	g.p("addi t0, t0, 1")
	g.p(".Latoi_digits:")
	g.p("li t5, 0")
	g.p(".Latoi_loop:")
	g.p("bge t0, t2, .Latoi_fin")
	g.p("lbu t3, 0(t1)")
	g.p("li t4, 48")
	g.p("blt t3, t4, .Latoi_fin")
	g.p("li t4, 57")
	g.p("blt t4, t3, .Latoi_fin")
	g.p("addi t3, t3, -48")
	g.p("li t4, 10")
	g.p("mul t5, t5, t4")
	g.p("add t5, t5, t3")
	g.p("addi t1, t1, 1")
	g.p("addi t0, t0, 1")
	g.p("j .Latoi_loop")
	g.p(".Latoi_fin:")
	g.p("mv a0, t5")
	g.p("beqz t6, .Latoi_ret")
	g.p("sub a0, zero, a0")
	g.p(".Latoi_ret:")
	g.p("ret")
	g.p(".Latoi_zero:")
	g.p("li a0, 0")
	g.p("ret")

	g.p("_rir_print_arena:")
	g.p("blt a0, zero, .Lpa_ret")
	g.p("addi sp, sp, -16")
	g.p("sw ra, 12(sp)")
	g.p("la t1, arena")
	g.p("add t1, t1, a0")
	g.p("li t2, %d", g.img.dsize)
	g.p("sub t2, t2, a0")
	g.p("mv t3, t1")
	g.p("li t4, 0")
	g.p(".Lpa_loop:")
	g.p("bge t4, t2, .Lpa_done")
	g.p("lbu t5, 0(t1)")
	g.p("beqz t5, .Lpa_done")
	g.p("addi t1, t1, 1")
	g.p("addi t4, t4, 1")
	g.p("j .Lpa_loop")
	g.p(".Lpa_done:")
	g.p("mv a1, t3")
	g.p("mv a2, t4")
	g.p("li a0, 1")
	g.p("li a7, 1")
	g.p("ecall")
	g.p("lw ra, 12(sp)")
	g.p("addi sp, sp, 16")
	g.p(".Lpa_ret:")
	g.p("ret")
}

func (g *gen) emitData() {
	g.p(".section .rodata,\"a\"")
	g.p(".Lbounds_msg:")
	g.p(".byte 98,111,117,110,100,115,10,0")
	g.p(".Lempty:")
	g.p(".byte 0")
	g.p(".balign 4")
	g.p(".Lp10:")
	g.p(".word 1,10,100,1000,10000")
	g.p(".balign 4")
	g.p("strpool:")
	if len(g.img.strpool) == 0 {
		g.p(".byte 0")
	} else {
		writeBytes(&g.b, g.img.strpool)
		g.p(".byte 0")
	}

	g.p(".section .data,\"aw\"")
	g.p(".balign 4")
	g.p("arena:")
	if len(g.img.arena) == 0 {
		g.p(".byte 0")
	} else {
		writeBytes(&g.b, g.img.arena)
	}

	g.p(".section .bss")
	g.p(".balign 4")
	g.p("g:")
	n := int(g.img.gsize)
	if n < 1 {
		n = 1
	}
	g.p(".zero %d", n*4)
	g.p(".balign 4")
	g.p("argbuf:")
	g.p(".zero 64")
	g.p(".balign 4")
	g.p("pwmreq:")
	g.p(".zero 12")
}

func writeBytes(b *strings.Builder, data []byte) {
	for i := 0; i < len(data); i += 24 {
		end := i + 24
		if end > len(data) {
			end = len(data)
		}
		b.WriteString(".byte ")
		for j, v := range data[i:end] {
			if j > 0 {
				b.WriteByte(',')
			}
			fmt.Fprintf(b, "%d", v)
		}
		b.WriteByte('\n')
	}
}

func buildELF(text, file string) ([]byte, error) {
	ob, err := as.Assemble([]byte(text), as.Opt{Arch: as.RV32, File: file})
	if err != nil {
		return nil, fmt.Errorf("assemble: %w", err)
	}
	elf, err := ld.Link([]*obj.Object{ob}, ld.Options{Machine: uint16(ld.EM_RISCV), Entry: "_start"})
	if err != nil {
		return nil, fmt.Errorf("link: %w", err)
	}
	return elf, nil
}

func main() {
	asmOut := flag.String("asm", "", "write the generated assembly here")
	elfOut := flag.String("elf", "", "write the raw ELF here as well")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "usage: irc [-asm file.s] [-elf file.elf] input.rir output.tos\n")
	}
	flag.Parse()
	if flag.NArg() != 2 {
		flag.Usage()
		os.Exit(2)
	}
	in, out := flag.Arg(0), flag.Arg(1)

	img, err := loadImage(in)
	if err != nil {
		fmt.Fprintf(os.Stderr, "irc: %v\n", err)
		os.Exit(1)
	}
	g := &gen{img: img}
	text, err := func() (text string, err error) {
		defer func() {
			if r := recover(); r != nil {
				err = fmt.Errorf("%v", r)
			}
		}()
		return g.gen()
	}()
	if err != nil {
		fmt.Fprintf(os.Stderr, "irc: %v\n", err)
		os.Exit(1)
	}
	if *asmOut != "" {
		if err := os.WriteFile(*asmOut, []byte(text), 0o644); err != nil {
			fmt.Fprintf(os.Stderr, "irc: %v\n", err)
			os.Exit(1)
		}
	}
	elf, err := buildELF(text, in+".s")
	if err != nil {
		fmt.Fprintf(os.Stderr, "irc: %v\n", err)
		os.Exit(1)
	}
	tmp, err := os.CreateTemp("", "irc-*.elf")
	if err != nil {
		fmt.Fprintf(os.Stderr, "irc: %v\n", err)
		os.Exit(1)
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if _, err := tmp.Write(elf); err != nil {
		tmp.Close()
		fmt.Fprintf(os.Stderr, "irc: %v\n", err)
		os.Exit(1)
	}
	tmp.Close()
	if *elfOut != "" {
		if err := os.WriteFile(*elfOut, elf, 0o644); err != nil {
			fmt.Fprintf(os.Stderr, "irc: %v\n", err)
			os.Exit(1)
		}
	}
	res, err := lz4tos.Pack(tmpName, out)
	if err != nil {
		fmt.Fprintf(os.Stderr, "irc: pack: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("%s -> %s (%d -> %d bytes, span %d)\n", filepath.Base(in),
		filepath.Base(out), res.RawSize, res.CompSize, res.Hint.Span)
}
