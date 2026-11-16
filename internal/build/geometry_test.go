package build

import (
	"os"
	"path/filepath"
	"regexp"
	"testing"
)

// TestWindowGeometryMatchesLayouts keeps the Go copy of the task-window
// geometry honest.  checkWindow() uses WindowGeometry() to reject programs
// at build time; when it drifted to 16K/8K the kernel was allocating 4K
// windows and the build happily accepted programs that exec() then rejected.
func TestWindowGeometryMatchesLayouts(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}

	cases := []struct {
		board string
		names []string
	}{
		{"esp32c3", []string{"PLAT_APP_GRAIN", "PLAT_STACK_BYTES", "PLAT_HEAP_GUARD"}},
		{"rp2040", []string{"PLAT_APP_GRAIN", "PLAT_STACK_BYTES", "PLAT_HEAP_GUARD"}},
		{"rp2350", []string{"PLAT_APP_GRAIN", "PLAT_STACK_BYTES", "PLAT_HEAP_GUARD"}},
		{"rp2350rv", []string{"PLAT_APP_GRAIN", "PLAT_STACK_BYTES", "PLAT_HEAP_GUARD"}},
	}

	for _, tc := range cases {
		t.Run(tc.board, func(t *testing.T) {
			src, err := os.ReadFile(filepath.Join(root, "board", tc.board, "layout.c"))
			if err != nil {
				t.Fatal(err)
			}
			got := make([]int, len(tc.names))
			for i, name := range tc.names {
				re := regexp.MustCompile(`(?m)^\s*(?:static\s+)?const\s+uint32_t\s+` +
					regexp.QuoteMeta(name) + `\s*=\s*([^;]+);`)
				m := re.FindStringSubmatch(string(src))
				if m == nil {
					t.Fatalf("no %s in layout.c", name)
				}
				v, err := evalIntExpr(m[1])
				if err != nil {
					t.Fatalf("%s: %v", name, err)
				}
				got[i] = v
			}
			grain, stack, guard := Target{Board: tc.board}.WindowGeometry()
			if grain != got[0] || stack != got[1] || guard != got[2] {
				t.Fatalf("WindowGeometry() = %d/%d/%d, layout.c = %d/%d/%d",
					grain, stack, guard, got[0], got[1], got[2])
			}
		})
	}
}
