package build

import (
	"bytes"
	"debug/elf"
	"encoding/binary"
	"os"
	"path/filepath"
	"testing"
)

// findUserELF locates a built user program, building one if necessary.
func findUserELF(t *testing.T) string {
	t.Helper()
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	// Prefer an already-built binary from a previous `tos build`.  The
	// converter only understands i386 ELF32, so skip when the build
	// directory holds another target's output.
	for _, name := range []string{"hello", "sh", "ls"} {
		p := filepath.Join(root, "build", "user", "bin", name)
		if _, err := os.Stat(p); err == nil && isI386ELF(p) {
			return p
		}
	}
	t.Skip("no built i386 user program (run 'tos build')")
	return ""
}

func TestLZ4RoundTrip(t *testing.T) {
	cases := [][]byte{
		{},
		{1},
		[]byte("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
		[]byte("the quick brown fox jumps over the lazy dog"),
		bytes.Repeat([]byte("ABC"), 1000),
	}
	// a semi-realistic binary-ish blob
	blob := make([]byte, 5000)
	for i := range blob {
		blob[i] = byte(i * 7 % 251)
	}
	cases = append(cases, blob)

	for i, in := range cases {
		dst := make([]byte, lz4CompressBound(len(in)))
		n := lz4CompressBlock(in, dst)
		out, err := lz4DecompressBlock(dst[:n], len(in))
		if err != nil {
			t.Fatalf("case %d: decompress: %v", i, err)
		}
		if !bytes.Equal(in, out) {
			t.Fatalf("case %d: round trip mismatch (%d in, %d out)", i, len(in), len(out))
		}
	}
}

func TestELFToMini(t *testing.T) {
	elfPath := findUserELF(t)
	img, err := ELFToMini(elfPath, true)
	if err != nil {
		t.Fatal(err)
	}

	// header sanity
	if len(img.Data) != MiniHeaderSize+int(img.PayloadLen) {
		t.Fatalf("container size %d != header %d + payload %d",
			len(img.Data), MiniHeaderSize, img.PayloadLen)
	}
	if string(img.Data[0:4]) != MiniMagic {
		t.Fatalf("bad magic %q", img.Data[0:4])
	}
	if img.Data[4] != MiniVersion {
		t.Fatalf("version = %d", img.Data[4])
	}
	if img.Entry < img.LoadAddr || img.Entry >= img.LoadAddr+img.MemSize {
		t.Fatalf("entry 0x%x outside [0x%x, 0x%x+0x%x)",
			img.Entry, img.LoadAddr, img.LoadAddr, img.MemSize)
	}

	// The payload must decode back to the file-backed bytes of the ELF.
	var payload []byte
	if img.Compressed {
		payload, err = lz4DecompressBlock(img.Data[MiniHeaderSize:], int(img.FileSize))
		if err != nil {
			t.Fatalf("decompress: %v", err)
		}
	} else {
		payload = img.Data[MiniHeaderSize:]
	}
	if uint32(len(payload)) != img.FileSize {
		t.Fatalf("decompressed %d bytes, want %d", len(payload), img.FileSize)
	}

	// Compare against the ELF's own PT_LOAD contents.
	f, err := elf.Open(elfPath)
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	raw, err := os.ReadFile(elfPath)
	if err != nil {
		t.Fatal(err)
	}
	want := make([]byte, img.FileSize)
	for _, p := range f.Progs {
		if p.Type != elf.PT_LOAD || p.Filesz == 0 {
			continue
		}
		off := p.Vaddr - uint64(img.LoadAddr)
		copy(want[off:], raw[p.Off:p.Off+p.Filesz])
	}
	if !bytes.Equal(payload, want) {
		t.Fatal("payload does not match ELF PT_LOAD contents")
	}

	// The compressed container must be smaller than the original ELF.
	st, _ := os.Stat(elfPath)
	if len(img.Data) >= int(st.Size()) {
		t.Errorf("mini container (%d) is not smaller than the ELF (%d)",
			len(img.Data), st.Size())
	}
	t.Logf("%s: ELF %d -> mini %d bytes (%.0f%%), mem=%d file=%d",
		filepath.Base(elfPath), st.Size(), len(img.Data),
		100*float64(len(img.Data))/float64(st.Size()), img.MemSize, img.FileSize)
}

func TestMiniHeaderFields(t *testing.T) {
	elfPath := findUserELF(t)
	img, err := ELFToMini(elfPath, false)
	if err != nil {
		t.Fatal(err)
	}
	got, err := ParseMini(img.Data)
	if err != nil {
		t.Fatal(err)
	}
	if got.Entry != img.Entry || got.LoadAddr != img.LoadAddr || got.MemSize != img.MemSize {
		t.Fatalf("parse mismatch: %+v vs %+v", got, img)
	}
	if got.Compressed {
		t.Fatal("expected uncompressed when compress=false")
	}
	if binary.LittleEndian.Uint32(img.Data[20:]) != img.PayloadLen {
		t.Fatal("payload length field mismatch")
	}
}
