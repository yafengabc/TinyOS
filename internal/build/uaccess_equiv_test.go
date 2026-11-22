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
// Lifetime: this is a before/after check for one commit, not a contract for
// all time.  If uaccess_range_ok's semantics are changed on purpose, delete
// this test in the same commit (and update #53's harness, which states the
// intended rules) - do not "fix" the harness copy to match, because then it
// stops being a characterisation of the move.  It stays until the extraction
// has been through a release cycle.
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
	build := func(out string, extra ...string) {
		t.Helper()
		args := append([]string{}, cc[1:]...)
		args = append(args, "-std=c99", "-Wall", "-Wextra", "-I", dir)
		args = append(args, extra...)
		args = append(args, filepath.Join(dir, "uaccess.c"),
			filepath.Join(dir, "harness.c"), "-o", out)
		cmd := exec.Command(cc[0], args...)
		cmd.Dir = dir
		if out, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("compiling the uaccess equivalence harness failed: %v\n%s", err, out)
		}
	}
	build(exe)

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

	// The harness's own negative: drop the reference's overflow branch.  If
	// the comparison is real, this build must disagree.  Without it the green
	// above could come from a harness that compares two identical copies.
	broken := exe + "-broken"
	build(broken, "-DBREAK_OVERFLOW_BRANCH")
	bad := exec.Command(broken)
	bad.Dir = dir
	out, err = bad.CombinedOutput()
	if err == nil {
		t.Fatalf("reference with the overflow branch removed still agreed; "+
			"the harness is not comparing anything:\n%s", out)
	}
	if !strings.Contains(string(out), "mismatches") {
		t.Fatalf("broken reference failed for the wrong reason:\n%s", out)
	}
	t.Logf("negative control: %s", strings.TrimSpace(string(out)))
}
