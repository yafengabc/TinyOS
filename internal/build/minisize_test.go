package build

import (
	"debug/elf"
	"os"
	"path/filepath"
	"testing"
)

// userBinDir is where `tos build` drops the user programs.  It holds one
// target's output at a time, so the x86-only checks below skip when the
// last build was for another architecture.
func userBinDir(t *testing.T) (string, bool) {
	t.Helper()
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	dir := filepath.Join(root, "build", "user", "bin")
	if _, err := os.ReadDir(dir); err != nil {
		t.Skip("run 'tos build' first")
	}
	return dir, true
}

// isI386ELF reports whether the file is an i386 ELF32 executable, i.e.
// something the MiniELF converter understands.
func isI386ELF(path string) bool {
	f, err := elf.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	return f.Class == elf.ELFCLASS32 && f.Machine == elf.EM_386
}

func TestMiniSizesAll(t *testing.T) {
	dir, ok := userBinDir(t)
	if !ok {
		return
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Skip("run 'tos build' first")
	}

	var totalELF, totalMini int64
	var n int
	t.Logf("%-10s %8s %8s %6s", "program", "ELF", "mini", "ratio")
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		p := filepath.Join(dir, e.Name())
		if !isI386ELF(p) {
			continue
		}
		st, err := os.Stat(p)
		if err != nil {
			continue
		}
		img, err := ELFToMini(p, true)
		if err != nil {
			t.Fatalf("%s: %v", e.Name(), err)
		}
		totalELF += st.Size()
		totalMini += int64(len(img.Data))
		n++
		t.Logf("%-10s %8d %8d %5.0f%%", e.Name(), st.Size(), len(img.Data),
			100*float64(len(img.Data))/float64(st.Size()))
	}
	if n == 0 {
		t.Skip("build/user/bin holds another target's output (run 'tos build')")
	}
	t.Logf("%-10s %8d %8d %5.0f%%", "TOTAL", totalELF, totalMini,
		100*float64(totalMini)/float64(totalELF))
}
