package build

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The window geometry is three numbers in board.conf that used to be mirrored
// by hand in layout.c and only reconciled by a panic at boot.  The two checks
// that need no link-time address are now build errors; these are their
// negative cases, plus a positive one so the rule cannot be made impossible
// to satisfy.
func TestBoardWindowGeometry(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Fatal(err)
	}
	// LoadBoardConfig looks under <root>/board, so the fixture has to live
	// there; the name is one no target claims.
	base := filepath.Join(root, "board", "zz-geometry-test")
	if err := os.MkdirAll(base, 0o755); err != nil {
		t.Skipf("cannot write under board/: %v", err)
	}
	defer os.RemoveAll(base)

	load := func(window string) error {
		t.Helper()
		conf := "[board]\nschema = 1\nchip = rp2350\nfs_backing = embedded\n" +
			"ramdisk_sectors = 1024\n[window]\n" + window
		if err := os.WriteFile(filepath.Join(base, "board.conf"), []byte(conf), 0o644); err != nil {
			t.Fatal(err)
		}
		_, err := LoadBoardConfig(root, "zz-geometry-test")
		return err
	}

	if err := load("grain = 4k\nstack = 4k\nguard = 4k\n"); err != nil {
		t.Fatalf("a sane geometry was rejected: %v", err)
	}
	cases := []struct {
		name, window, want string
	}{
		{"grain not a power of two", "grain = 12k\nstack = 4k\nguard = 4k\n", "power of two"},
		{"stack not a power of two", "grain = 8k\nstack = 6k\nguard = 1k\n", "power of two"},
		{"stack below a page", "grain = 4k\nstack = 2k\nguard = 1k\n", "at least 4096"},
		{"stack does not divide the grain", "grain = 4k\nstack = 8k\nguard = 1k\nstack_align = 1\n", "multiple of stack"},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			err := load(c.window)
			if err == nil {
				t.Fatalf("%s was accepted", c.name)
			}
			if !strings.Contains(err.Error(), c.want) {
				t.Fatalf("error should mention %q, got %q", c.want, err)
			}
		})
	}
	// Without stack_align the same pair is legal: those ports give the stack
	// its own region instead of encoding it as one aligned entry.
	if err := load("grain = 4k\nstack = 8k\nguard = 1k\n"); err != nil {
		t.Fatalf("no stack_align should not require divisibility: %v", err)
	}
}
