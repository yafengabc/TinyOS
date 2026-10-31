package build

import (
	"os"
	"path/filepath"
	"testing"
)

// The LZ4TOS envelope the kernel's lz4_wrap_info() recognises
// (kernel/lz4.h).  The build side does not have these yet.
const (
	lz4WrapMagic  = "LZ4TOS"
	lz4WrapHeader = 16
)

// TestL2Sizes measures what every user program would weigh if the build
// wrapped it the way kernel/lz4.c expects (LZ4TOS header + raw LZ4 block).
// The kernel already understands this envelope (process.c:lz4_wrap_info),
// but the build never produces it, so L2 is currently dormant.
func TestL2Sizes(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	dir := filepath.Join(root, "build", "user", "bin")
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Skip("run 'tos build' first")
	}

	var totalRaw, totalL2, totalMini int64
	t.Logf("%-10s %8s %8s %8s %7s", "program", "raw", "L2", "MiniELF", "L2/raw")

	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		p := filepath.Join(dir, e.Name())
		raw, err := os.ReadFile(p)
		if err != nil {
			t.Fatalf("%s: %v", e.Name(), err)
		}

		// LZ4TOS envelope: 6 byte magic, 2 reserved, 4 raw size, 4 block size.
		dst := make([]byte, lz4CompressBound(len(raw)))
		n := lz4CompressBlock(raw, dst)
		if n <= 0 {
			t.Fatalf("%s: compression failed", e.Name())
		}
		l2 := lz4WrapHeader + n

		mini := 0
		if img, err := ELFToMini(p, true); err == nil {
			mini = len(img.Data)
		}

		totalRaw += int64(len(raw))
		totalL2 += int64(l2)
		totalMini += int64(mini)
		t.Logf("%-10s %8d %8d %8d %6.0f%%", e.Name(), len(raw), l2, mini,
			100*float64(l2)/float64(len(raw)))
	}

	t.Logf("%-10s %8d %8d %8d %6.0f%%", "TOTAL", totalRaw, totalL2, totalMini,
		100*float64(totalL2)/float64(totalRaw))
}
