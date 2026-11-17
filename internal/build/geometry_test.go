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

	/* Every board with a layout.c, not a hand-picked four: a new board that
	 * only edits board.conf + layout.c must be covered automatically. */
	boards, err := filepath.Glob(filepath.Join(root, "board", "*", "layout.c"))
	if err != nil {
		t.Fatal(err)
	}
	names := []string{"PLAT_APP_GRAIN", "PLAT_STACK_BYTES", "PLAT_HEAP_GUARD"}

	for _, file := range boards {
		board := filepath.Base(filepath.Dir(file))
		t.Run(board, func(t *testing.T) {
			src, err := os.ReadFile(file)
			if err != nil {
				t.Fatal(err)
			}
			got := make([]int, len(names))
			for i, name := range names {
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
			/* Enrich a target for this board the same way a build does. */
			var tg Target
			for _, cand := range Targets {
				if cand.Board == board {
					tg = cand
					break
				}
			}
			if tg.Name == "" {
				t.Fatalf("no Target for board %q", board)
			}
			b := New(root)
			b.Target = tg
			if err := b.ensureBoardConfig(); err != nil {
				t.Fatal(err)
			}
			grain, stack, guard := b.Target.WindowGeometry()
			if grain != got[0] || stack != got[1] || guard != got[2] {
				t.Fatalf("WindowGeometry() = %d/%d/%d, layout.c = %d/%d/%d",
					grain, stack, guard, got[0], got[1], got[2])
			}
			bc, err := LoadBoardConfig(root, board)
			if err != nil {
				t.Fatal(err)
			}
			if bc.WindowGrain != got[0] || bc.WindowStack != got[1] || bc.WindowGuard != got[2] {
				t.Fatalf("board.conf window = %d/%d/%d, layout.c = %d/%d/%d",
					bc.WindowGrain, bc.WindowStack, bc.WindowGuard, got[0], got[1], got[2])
			}
		})
	}
}
