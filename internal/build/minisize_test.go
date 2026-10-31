package build

import (
	"os"
	"path/filepath"
	"testing"
)

func TestMiniSizesAll(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	dir := filepath.Join(root, "build", "user", "bin")
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Skip("run 'tos build' first")
	}

	var totalELF, totalMini int64
	t.Logf("%-10s %8s %8s %6s", "program", "ELF", "mini", "ratio")
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		p := filepath.Join(dir, e.Name())
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
		t.Logf("%-10s %8d %8d %5.0f%%", e.Name(), st.Size(), len(img.Data),
			100*float64(len(img.Data))/float64(st.Size()))
	}
	t.Logf("%-10s %8d %8d %5.0f%%", "TOTAL", totalELF, totalMini,
		100*float64(totalMini)/float64(totalELF))
}
