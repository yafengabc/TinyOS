package build

import (
	"bytes"
	"debug/elf"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

// makeTestELF builds a little-endian ELF32 with one PT_LOAD covering the
// header, the program header table and a short payload, then an unreferenced
// tail (a stand-in for .shstrtab) and a two-entry section header table at EOF.
func makeTestELF() []byte {
	const (
		ehsize    = 52
		phentsize = 32
		phnum     = 1
		shentsize = 40
		shnum     = 2
		loadEnd   = 108 // header + phdr + 24 bytes of payload
		shoff     = 116 // 8 bytes of "shstrtab" in between
	)
	d := make([]byte, shoff+shnum*shentsize)
	copy(d, []byte{0x7f, 'E', 'L', 'F', 1, 1, 1})
	le := binary.LittleEndian
	le.PutUint16(d[16:], 2) // ET_EXEC
	le.PutUint16(d[18:], 3) // EM_386
	le.PutUint32(d[20:], 1)
	le.PutUint32(d[28:], ehsize)
	le.PutUint32(d[32:], shoff)
	le.PutUint16(d[40:], ehsize)
	le.PutUint16(d[42:], phentsize)
	le.PutUint16(d[44:], phnum)
	le.PutUint16(d[46:], shentsize)
	le.PutUint16(d[48:], shnum)
	le.PutUint16(d[50:], 1) // e_shstrndx

	p := d[ehsize:]
	le.PutUint32(p[0:], 1) // PT_LOAD
	le.PutUint32(p[4:], 0)
	le.PutUint32(p[8:], 0x1000)
	le.PutUint32(p[12:], 0x1000)
	le.PutUint32(p[16:], loadEnd)
	le.PutUint32(p[20:], loadEnd)
	le.PutUint32(p[24:], 5)
	le.PutUint32(p[28:], 0x1000)
	for i := ehsize + phentsize; i < loadEnd; i++ {
		d[i] = byte(i)
	}
	copy(d[loadEnd:shoff], []byte(".shstrtab\x00")[:8])

	// Section 1 describes that string table; section 0 is the null entry.
	s := d[shoff+shentsize:]
	le.PutUint32(s[0:], 1)
	le.PutUint32(s[4:], 3) // SHT_STRTAB
	le.PutUint32(s[16:], loadEnd)
	le.PutUint32(s[20:], 8)
	return d
}

func writeTempELF(t *testing.T, d []byte) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "prog")
	if err := os.WriteFile(p, d, 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestTrimELFSections(t *testing.T) {
	orig := makeTestELF()
	p := writeTempELF(t, orig)

	saved, err := TrimELFSections(p)
	if err != nil {
		t.Fatal(err)
	}
	if saved != 88 {
		t.Fatalf("saved %d bytes, want 88", saved)
	}
	got, err := os.ReadFile(p)
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 108 {
		t.Fatalf("trimmed file is %d bytes, want 108 (last byte any phdr needs)", len(got))
	}
	want := bytes.Clone(orig[:108])
	for _, off := range []int{32, 46, 48, 50} {
		want[off], want[off+1] = 0, 0
	}
	if !bytes.Equal(got, want) {
		t.Fatal("bytes below the trim point changed (other than the section header fields)")
	}
	for _, off := range []int{32, 46, 48, 50} {
		if got[off] != 0 || got[off+1] != 0 {
			t.Fatalf("ELF header field at %d was not zeroed", off)
		}
	}

	// debug/elf is what the build's size/corruption tests use on these files.
	f, err := elf.Open(p)
	if err != nil {
		t.Fatalf("stripped ELF no longer opens: %v", err)
	}
	if len(f.Progs) != 1 || f.Progs[0].Filesz != 108 {
		t.Fatalf("program header damaged: %+v", f.Progs)
	}
	f.Close()

	// Idempotent: a second pass must not touch the file again.
	if n, err := TrimELFSections(p); err != nil || n != 0 {
		t.Fatalf("second trim returned (%d, %v), want (0, nil)", n, err)
	}
	again, _ := os.ReadFile(p)
	if !bytes.Equal(again, got) {
		t.Fatal("second trim changed the file")
	}
}

func TestTrimELFSectionsKeepsUnknownLayouts(t *testing.T) {
	le := binary.LittleEndian

	t.Run("section table not at EOF", func(t *testing.T) {
		d := append(makeTestELF(), 0xaa)
		p := writeTempELF(t, d)
		if n, err := TrimELFSections(p); err != nil || n != 0 {
			t.Fatalf("returned (%d, %v), want (0, nil)", n, err)
		}
		got, _ := os.ReadFile(p)
		if !bytes.Equal(got, d) {
			t.Fatal("file was modified")
		}
	})

	t.Run("a program header reaches into the tail", func(t *testing.T) {
		d := makeTestELF()
		le.PutUint32(d[52+16:], 130) // p_filesz overlaps the section table
		p := writeTempELF(t, d)
		if n, err := TrimELFSections(p); err != nil || n != 0 {
			t.Fatalf("returned (%d, %v), want (0, nil)", n, err)
		}
		got, _ := os.ReadFile(p)
		if !bytes.Equal(got, d) {
			t.Fatal("file was modified")
		}
	})

	t.Run("not an ELF", func(t *testing.T) {
		p := writeTempELF(t, []byte("hello, world"))
		if n, err := TrimELFSections(p); err != nil || n != 0 {
			t.Fatalf("returned (%d, %v), want (0, nil)", n, err)
		}
	})
}

// TestStagedProgramsHaveNoSections checks the real build output (whichever
// target was built last): every ELF staged into the littlefs image must have
// lost its section table and must still be readable by debug/elf, which is
// what the other build tests use.
func TestStagedProgramsHaveNoSections(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	dir := filepath.Join(root, "build", "fs-stage", "bin")
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Skip("run 'tos build' first")
	}

	n := 0
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		p := filepath.Join(dir, e.Name())
		d, err := os.ReadFile(p)
		if err != nil {
			t.Fatal(err)
		}
		if len(d) < 52 || !bytes.HasPrefix(d, []byte("\x7fELF")) {
			continue
		}
		n++
		shoff := binary.LittleEndian.Uint32(d[32:])
		shentsize := binary.LittleEndian.Uint16(d[46:])
		shnum := binary.LittleEndian.Uint16(d[48:])
		shstrndx := binary.LittleEndian.Uint16(d[50:])
		if shoff != 0 || shentsize != 0 || shnum != 0 || shstrndx != 0 {
			t.Errorf("%s still has a section table (shoff=%d shnum=%d)", e.Name(), shoff, shnum)
		}
		f, err := elf.Open(p)
		if err != nil {
			t.Errorf("%s no longer opens with debug/elf: %v", e.Name(), err)
			continue
		}
		if len(f.Progs) == 0 {
			t.Errorf("%s has no program headers", e.Name())
		}
		f.Close()
	}
	if n == 0 {
		t.Skip("no ELF user programs staged")
	}
}
