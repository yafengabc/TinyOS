package build

import (
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"strconv"
	"strings"
	"testing"
)

// The RP2350 RISC-V half describes a task window with NAPOT PMP blocks, and
// the part only has seven usable regions.  Whether a given window fits is
// arithmetic, and it stopped fitting for csc's 112 KiB window once the grain
// went to 16 KiB (issue #44: nine blocks with a page-granular split).  The
// board cannot run on a build machine, so compile the real mapper against a
// host harness that feeds it the measured geometry and sweeps the range of
// page-granular splits.
func TestRP2350PmpWindowMap(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	cc, err := hostCC()
	if err != nil {
		t.Skip(err)
	}

	dir := t.TempDir()
	copyFile(t, filepath.Join(root, "board", "rp2350rv-generic", "pmp_map.h"),
		filepath.Join(dir, "pmp_map.h"))
	copyFile(t, filepath.Join(root, "internal", "build", "testdata", "pmp_map_harness.c"),
		filepath.Join(dir, "harness.c"))

	exe := filepath.Join(dir, "harness")
	if runtime.GOOS == "windows" {
		exe += ".exe"
	}
	build := exec.Command(cc[0], append(cc[1:], "-std=c99", "-Wall", "-Wextra",
		"-I", dir, filepath.Join(dir, "harness.c"), "-o", exe)...)
	build.Dir = dir
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("compiling the PMP mapper harness failed: %v\n%s", err, out)
	}

	cmd := exec.Command(exe)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	text := string(out)
	if err != nil {
		t.Fatalf("the PMP window mapper broke an invariant: %v\n%s", err, text)
	}

	// The regression this test exists for: the geometry that used to print
	// "needs more NAPOT blocks than there are regions" on every switch.
	for _, want := range []string{
		"rcc: SPLIT does not fit, SNAP chosen",
		"csc: SPLIT kept",
		"tiny: SPLIT kept",
		"area: SPLIT kept",
		"rcc-area: ",
		"oversized: refused",
		"ok",
	} {
		if !strings.Contains(text, want) {
			t.Errorf("harness output is missing %q\n%s", want, text)
		}
	}
	t.Logf("%s", strings.TrimSpace(text))
}

// Guard against the harness and the board drifting apart: the host test is
// only meaningful with the board's real grain, which `x` and the wheel both
// read as a literal.
func TestRP2350PmpMapGrain(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	layout, err := os.ReadFile(filepath.Join(root, "board", "rp2350rv-generic", "layout.c"))
	if err != nil {
		t.Fatal(err)
	}
	harness, err := os.ReadFile(filepath.Join(root, "internal", "build", "testdata", "pmp_map_harness.c"))
	if err != nil {
		t.Fatal(err)
	}
	re := regexp.MustCompile(`PLAT_APP_GRAIN\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)u`)
	value := func(src []byte, where string) uint64 {
		m := re.FindSubmatch(src)
		if m == nil {
			t.Fatalf("%s: PLAT_APP_GRAIN not found", where)
		}
		v, err := strconv.ParseUint(string(m[1]), 0, 32)
		if err != nil {
			t.Fatalf("%s: %v", where, err)
		}
		return v
	}
	if got, want := value(harness, "harness"), value(layout, "layout.c"); got != want {
		t.Fatalf("the harness maps with a %d-byte grain, the board uses %d", got, want)
	}
	if want := uint64(16 * 1024); value(layout, "layout.c") != want {
		t.Fatalf("board/rp2350rv-generic/layout.c no longer carries the 16 KiB grain the mapper is tuned for")
	}
}
