package build

import (
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// TestWindowPlacementLayout guards issue #55: a 128 KiB window on the C3's
// 4 KiB grain must fit the 224 KiB gap left by msh.  The placement rule is a
// pure header (kernel/win_layout.h), so the real code compiles on the host
// with no kernel dependencies; the harness also runs the old rule to prove
// the regression tuple actually distinguishes them.
func TestWindowPlacementLayout(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Fatalf("not in the source tree: %v", err)
	}
	cc, err := hostCC()
	if err != nil {
		t.Fatalf("host C compiler unavailable: %v (set TOS_CC)", err)
	}

	dir := t.TempDir()
	copyFile(t, filepath.Join(root, "kernel", "win_layout.h"), filepath.Join(dir, "win_layout.h"))
	copyFile(t, filepath.Join(root, "internal", "build", "testdata", "win_layout_harness.c"),
		filepath.Join(dir, "harness.c"))

	exe := filepath.Join(dir, "harness")
	if runtime.GOOS == "windows" {
		exe += ".exe"
	}
	build := exec.Command(cc[0], append(cc[1:], "-std=c99", "-Wall", "-Wextra",
		"-I", dir, filepath.Join(dir, "harness.c"), "-o", exe)...)
	build.Dir = dir
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("compiling the window layout harness failed: %v\n%s", err, out)
	}

	cmd := exec.Command(exe)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("window placement broke: %v\n%s", err, out)
	}
	if !strings.Contains(string(out), "win layout: ok") {
		t.Fatalf("harness did not report success:\n%s", out)
	}
	t.Logf("%s", strings.TrimSpace(string(out)))
}
