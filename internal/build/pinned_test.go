package build

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

// TestPinnedToolchainSources fails when the board-side copy of the toolchain
// drifts from the sibling tinyos-script checkout.
//
// TinyOS carries a pinned copy of csc/rvm/rcc so an image builds without the
// other repository.  Nothing generates that copy: it is a manual `cp`, which
// means a change made on one side silently ships stale code on the other.  The
// RP2040 Thumb-1 work is exactly the kind of change that spans both trees, so
// the mapping from port/tinyos/README.md is checked here.
//
// Skipped when ../tinyos-script is not checked out (CI builds TinyOS alone).
func TestPinnedToolchainSources(t *testing.T) {
	root := filepath.Join("..", "..")
	src := filepath.Join(root, "..", "tinyos-script", "src")
	if _, err := os.Stat(src); err != nil {
		t.Skip("tinyos-script checkout not present")
	}
	pairs := [][2]string{
		{"csc.c", "user/csc/csc.c"},
		{"rvm.c", "user/rvm/rvm.c"},
		{filepath.Join("rcc", "rcc.c"), filepath.Join("user", "rcc", "rcc.c")},
		{filepath.Join("rcc", "rv32.inc"), filepath.Join("user", "rcc", "rv32.inc")},
		{filepath.Join("rcc", "thumb2.inc"), filepath.Join("user", "rcc", "thumb2.inc")},
		{filepath.Join("rcc", "thumb1.inc"), filepath.Join("user", "rcc", "thumb1.inc")},
	}
	for _, p := range pairs {
		a, err := os.ReadFile(filepath.Join(src, p[0]))
		if err != nil {
			t.Fatalf("pinned source missing: %v", err)
		}
		b, err := os.ReadFile(filepath.Join(root, p[1]))
		if err != nil {
			t.Fatalf("pinned copy missing: %v", err)
		}
		if !bytes.Equal(a, b) {
			t.Errorf("%s differs from tinyos-script/src/%s; copy it across and commit both",
				p[1], p[0])
		}
	}
}
