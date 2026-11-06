package build

import (
	"bytes"
	"debug/elf"
	"encoding/binary"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"testing"
)

// refWindowHint is an independent Go reading of the rules kernel/elf.c uses
// in elf_probe() and elf_tos_note().  It deliberately does not share code
// with tools/mklfs.c's elf_window_hint(), so a disagreement between the two
// catches a build-side bug instead of confirming it.
func refWindowHint(path string) (span, stack, heap uint32, err error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return 0, 0, 0, err
	}
	f, err := elf.NewFile(bytes.NewReader(raw))
	if err != nil {
		return 0, 0, 0, err
	}
	defer f.Close()

	var lo, hi uint64 = ^uint64(0), 0
	for _, p := range f.Progs {
		if p.Type != elf.PT_LOAD || p.Memsz == 0 {
			continue
		}
		if p.Vaddr < lo {
			lo = p.Vaddr
		}
		if p.Vaddr+p.Memsz > hi {
			hi = p.Vaddr + p.Memsz
		}
	}
	if hi <= lo {
		return 0, 0, 0, fmt.Errorf("image has no usable PT_LOAD")
	}
	span = uint32(((hi - lo) + 4095) / 4096 * 4096)

	for _, p := range f.Progs {
		if p.Type != elf.PT_NOTE || p.Off+p.Filesz > uint64(len(raw)) {
			continue
		}
		b := raw[p.Off : p.Off+p.Filesz]
		for uint64(len(b)) >= 12 {
			namesz := binary.LittleEndian.Uint32(b)
			descsz := binary.LittleEndian.Uint32(b[4:])
			ntype := binary.LittleEndian.Uint32(b[8:])
			descOff := uint64(12) + ((uint64(namesz) + 3) &^ 3)
			next := descOff + ((uint64(descsz) + 3) &^ 3)
			if descOff < 12 || next > uint64(len(b)) || next <= 12 {
				break
			}
			if namesz >= 6 && descsz >= 4 &&
				uint64(len(b)) >= 18 &&
				string(b[12:18]) == "TinyOS" {
				v := binary.LittleEndian.Uint32(b[descOff:])
				if ntype == 1 && stack == 0 {
					stack = v
				} else if ntype == 2 && heap == 0 {
					heap = v
				}
			}
			b = b[next:]
		}
	}
	return span, stack, heap, nil
}

// windowHintTool prefers the very mklfs binary the build chain produced -
// that is the one that wrote the real image - and only falls back to
// compiling a private copy when no build has run yet.  A stale binary would
// make the test agree with itself, so refuse one older than its source.
func windowHintTool(t *testing.T, dir string) string {
	t.Helper()
	root, err := FindRoot()
	if err != nil {
		t.Fatal(err)
	}
	tool := filepath.Join(root, "build", "mklfs.exe")
	src := filepath.Join(root, "tools", "mklfs.c")
	ts, err1 := os.Stat(tool)
	ss, err2 := os.Stat(src)
	if err1 == nil && err2 == nil && !ts.ModTime().Before(ss.ModTime()) {
		return tool
	}
	return buildMklfsTool(t, dir)
}

var hintLine = regexp.MustCompile(`(?m)^\s*(bin/\S+)\s+\d+ bytes\s+hint=(\d+)/(\d+)/(\d+)`)

// TestMklfsWindowHint packs the real staged ELFs into a throwaway littlefs
// image, reads the hints back through `mklfs -v`, and compares every one of
// them with an independently computed reference.  This is the end-to-end
// check that the values exec() will trust really describe the ELF.
func TestMklfsWindowHint(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	src := filepath.Join(root, "build", "fs-stage", "bin")
	entries, err := os.ReadDir(src)
	if err != nil {
		t.Skip("run 'tos build' first")
	}

	dir := t.TempDir()
	mklfs := windowHintTool(t, dir)
	stage := filepath.Join(dir, "stage")
	var names []string
	var elfs []string
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		data, err := os.ReadFile(filepath.Join(src, e.Name()))
		if err != nil {
			t.Fatal(err)
		}
		if len(data) < 4 || string(data[:4]) != "\x7fELF" {
			continue
		}
		dst := filepath.Join(stage, "bin", e.Name())
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(dst, data, 0o644); err != nil {
			t.Fatal(err)
		}
		names = append(names, "bin/"+e.Name())
		elfs = append(elfs, e.Name())
	}
	if len(elfs) == 0 {
		t.Skip("no staged ELF programs")
	}

	// A file that looks like an ELF to looks_like_elf() but is not a
	// parseable ELF32 must fall back to the old 16-byte envelope.
	stale := "\x7fELF-not-a-real-program"
	stalePath := filepath.Join(stage, "bin", "stale")
	if err := os.WriteFile(stalePath, []byte(stale), 0o644); err != nil {
		t.Fatal(err)
	}
	names = append(names, "bin/stale")

	img := filepath.Join(dir, "lfs.img")
	args := append([]string{img, "307200", stage}, names...)
	if b, err := exec.Command(mklfs, args...).CombinedOutput(); err != nil {
		t.Fatalf("mklfs create failed: %v\n%s", err, b)
	}
	out, err := exec.Command(mklfs, "-v", img).CombinedOutput()
	if err != nil {
		t.Fatalf("mklfs verify failed: %v\n%s", err, out)
	}

	got := map[string][3]uint32{}
	for _, m := range hintLine.FindAllStringSubmatch(string(out), -1) {
		var v [3]uint32
		for i := 0; i < 3; i++ {
			n, err := strconv.ParseUint(m[i+2], 10, 32)
			if err != nil {
				t.Fatalf("%s: bad hint %q: %v", m[1], m[i+2], err)
			}
			v[i] = uint32(n)
		}
		got[filepath.ToSlash(m[1])] = v
	}

	for _, name := range elfs {
		wantSpan, wantStack, wantHeap, err := refWindowHint(filepath.Join(src, name))
		if err != nil {
			t.Fatalf("%s: reference hint: %v", name, err)
		}
		v, ok := got["bin/"+name]
		if !ok {
			t.Errorf("%s: no window hint in the image", name)
			continue
		}
		want := [3]uint32{wantSpan, wantStack, wantHeap}
		if v != want {
			t.Errorf("%s: hint %v, want %v", name, v, want)
		}
	}
	if _, ok := got["bin/stale"]; ok {
		t.Error("an unparseable ELF got a hint; it must keep the old 16-byte envelope")
	}
	if len(got) != len(elfs) {
		t.Errorf("image reports %d hints, want %d", len(got), len(elfs))
	}
	t.Logf("verified %d window hints", len(got))
}

// TestWindowHintSpanIsNotFileSize pins down why original_size cannot stand
// in for the window hint: the two are unrelated for every program that
// declares a heap or has a .bss larger than its file bytes.
func TestWindowHintSpanIsNotFileSize(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	src := filepath.Join(root, "build", "fs-stage", "bin")
	if _, err := os.Stat(src); err != nil {
		t.Skip("run 'tos build' first")
	}
	worse := ""
	var ratio float64
	entries, _ := os.ReadDir(src)
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		p := filepath.Join(src, e.Name())
		st, err := os.Stat(p)
		if err != nil {
			continue
		}
		span, stack, heap, err := refWindowHint(p)
		if err != nil {
			continue
		}
		need := uint64(span) + uint64(stack) + uint64(heap) + 4096
		if r := float64(need) / float64(st.Size()); r > ratio {
			ratio, worse = r, e.Name()
		}
	}
	if ratio < 2 {
		t.Fatalf("expected a window much larger than some program's file, got %.1fx (%s)",
			ratio, worse)
	}
	t.Logf("largest window/file ratio %.1fx (%s)", ratio, worse)
}
