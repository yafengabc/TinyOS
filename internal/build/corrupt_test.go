package build_test

import (
	goelf "debug/elf"
	"os"
	"testing"

	"tinyos/internal/build"
)

// TestCorruptDetected flips bytes in the payload and checks the decode fails
// or produces different output.  LZ4 has no checksum, so a flipped byte in a
// literal run simply changes the output rather than raising an error; what
// matters is that it never silently produces the *original* bytes.
func TestCorruptDetected(t *testing.T) {
	root, err := build.FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	elfPath := root + "/build/user/bin/hello"
	if _, err := os.Stat(elfPath); err != nil {
		t.Skip("run 'tos build' first")
	}
	// ELFToMini only handles i386 ELF32; skip when the build directory
	// holds another target's output.
	if f, err := goelf.Open(elfPath); err != nil ||
		f.Class != goelf.ELFCLASS32 || f.Machine != goelf.EM_386 {
		t.Skip("build/user/bin holds another target's output (run 'tos build')")
	} else {
		f.Close()
	}

	img, err := build.ELFToMini(elfPath, true)
	if err != nil {
		t.Fatal(err)
	}
	good, err := build.VerifyMini(img)
	if err != nil {
		t.Fatalf("clean container fails to verify: %v", err)
	}

	decodedOriginal := decodePayload(t, img)

	flipped := 0
	for off := build.MiniHeaderSize + 1; off < len(img.Data); off += 7 {
		data := append([]byte(nil), img.Data...)
		data[off] ^= 0xFF
		bad, err := build.ParseMini(data)
		if err != nil {
			continue
		}
		if _, err := build.VerifyMini(bad); err != nil {
			flipped++
			continue
		}
		out := decodePayload(t, bad)
		if string(out) == string(decodedOriginal) {
			t.Fatalf("corruption at offset %d went completely undetected", off)
		}
		flipped++
	}
	if flipped == 0 {
		t.Fatal("no corruption cases were exercised")
	}
	t.Logf("checked %d corrupted containers (%d decoded bytes)", flipped, good)
}

func decodePayload(t *testing.T, img *build.MiniImage) []byte {
	t.Helper()
	out, err := build.DecodeMiniPayload(img)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	return out
}
