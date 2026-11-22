package build

import (
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// TestUaccessExtractionEquivalent is the characterisation half of #54: the
// old implementations (copied into the harness) and the real extracted ones
// must agree on every boundary and pseudo-random tuple.  This proves the move
// changed no behaviour; #53 owns the rule-level host tests.
//
// No t.Skip: if there is no host compiler, that is a failure, not a reason to
// silently lose the equivalence evidence.
func TestUaccessExtractionEquivalent(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Fatalf("not in the source tree: %v", err)
	}
	cc, err := hostCC()
	if err != nil {
		t.Fatalf("host C compiler unavailable: %v (set TOS_CC)", err)
	}

	dir := t.TempDir()
	for _, name := range []string{
		filepath.Join("kernel", "uaccess.c"),
		filepath.Join("kernel", "uaccess.h"),
		filepath.Join("kernel", "u32math.h"),
		filepath.Join("kernel", "types.h"),
	} {
		copyFile(t, filepath.Join(root, name), filepath.Join(dir, filepath.Base(name)))
	}
	copyFile(t, filepath.Join(root, "internal", "build", "testdata", "uaccess_equiv_harness.c"),
		filepath.Join(dir, "harness.c"))

	exe := filepath.Join(dir, "harness")
	if runtime.GOOS == "windows" {
		exe += ".exe"
	}
	build := exec.Command(cc[0], append(cc[1:], "-std=c99", "-Wall", "-Wextra",
		"-I", dir,
		filepath.Join(dir, "uaccess.c"), filepath.Join(dir, "harness.c"),
		"-o", exe)...)
	build.Dir = dir
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("compiling the uaccess equivalence harness failed: %v\n%s", err, out)
	}

	cmd := exec.Command(exe)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("uaccess/u32math behaviour changed during the move: %v\n%s", err, out)
	}
	if !strings.Contains(string(out), "equivalence: ok") {
		t.Fatalf("harness did not report success:\n%s", out)
	}
	t.Logf("%s", strings.TrimSpace(string(out)))
}
