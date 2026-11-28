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

	/* Every board, not a hand-picked four: a concrete board inherits
	 * layout.c through its Bases chain, and the test has to compare the
	 * value it will actually compile against the value its board.conf
	 * yields (including an inherited stack_area). */
	dirs, err := os.ReadDir(filepath.Join(root, "board"))
	if err != nil {
		t.Fatal(err)
	}
	var boards []string
	for _, d := range dirs {
		if d.IsDir() {
			boards = append(boards, filepath.Join(root, "board", d.Name()))
		}
	}
	names := []string{"PLAT_APP_GRAIN", "PLAT_STACK_BYTES", "PLAT_HEAP_GUARD"}
	/* Optional, because a port that keeps the stack in the data window has
	 * no PLAT_STACK_AREA and the weak default is 0.  A board that defines it
	 * must agree with [window] stack_area, and its user-area top must divide
	 * the 16 KiB growth step (layout_check() panics otherwise). */
	const areaStep = 4 * 4096
	areaOf := func(src, file string) int {
		re := regexp.MustCompile(`(?m)^\s*(?:static\s+)?const\s+uint32_t\s+PLAT_STACK_AREA\s*=\s*([^;]+);`)
		m := re.FindStringSubmatch(src)
		if m == nil {
			return 0
		}
		v, err := evalIntExpr(m[1])
		if err != nil {
			t.Fatalf("%s: PLAT_STACK_AREA: %v", file, err)
		}
		return v
	}
	uintOf := func(src, file, name string) int {
		re := regexp.MustCompile(`(?m)^\s*(?:static\s+)?const\s+uint32_t\s+` +
			regexp.QuoteMeta(name) + `\s*=\s*([^;]+);`)
		m := re.FindStringSubmatch(src)
		if m == nil {
			t.Fatalf("no %s in %s", name, file)
		}
		v, err := evalIntExpr(m[1])
		if err != nil {
			t.Fatalf("%s: %s: %v", file, name, err)
		}
		return v
	}

	for _, dir := range boards {
		board := filepath.Base(dir)
		t.Run(board, func(t *testing.T) {
			bc0, err := LoadBoardConfig(root, board)
			if err != nil {
				t.Fatal(err)
			}
			file := filepath.Join(dir, "layout.c")
			if _, err := os.Stat(file); err != nil {
				file = ""
				for _, base := range bc0.Bases {
					cand := filepath.Join(root, "board", base, "layout.c")
					if _, err := os.Stat(cand); err == nil {
						file = cand
						break
					}
				}
			}
			if file == "" {
				t.Skip("no layout.c in this board's base chain")
			}
			src, err := os.ReadFile(file)
			if err != nil {
				t.Fatal(err)
			}
			got := make([]int, len(names))
			for i, name := range names {
				got[i] = uintOf(string(src), file, name)
			}
			area := areaOf(string(src), file)
			if area != 0 {
				if area%areaStep != 0 || area < areaStep || area > 32*4096 {
					t.Fatalf("PLAT_STACK_AREA = %d: not a 16..128 KiB multiple of %d",
						area, areaStep)
				}
				if appMax := uintOf(string(src), file, "PLAT_APP_MAX"); appMax%areaStep != 0 {
					t.Fatalf("PLAT_APP_MAX = 0x%x is not %d KiB aligned, and the stack area is at the top",
						appMax, areaStep/1024)
				}
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
				/* A chip baseline (board/rp2350-generic) has no target of its
				 * own: its boards are the concrete ones.  Validate its conf
				 * with a synthetic target.  The ISA has to be one the chip
				 * has, or ensureBoardConfig rejects the target before the
				 * geometry is read. */
				arch := ""
				if isas := Chips[bc0.Chip].ISAs; len(isas) > 0 {
					arch = "arm"
					if _, ok := isas[arch]; !ok {
						for name := range isas {
							arch = name
							break
						}
					}
				}
				tg = Target{Name: board, Board: board, Arch: arch}
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
			if b.Target.WindowStackArea() != area || bc.WindowStackArea != area {
				t.Fatalf("board.conf stack_area = %d, layout.c = %d (target %d)",
					bc.WindowStackArea, area, b.Target.WindowStackArea())
			}
		})
	}
}
