package build

import (
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// The task-window heap is a free list in userland; the only kernel service it
// uses is sbrk(), so the whole thing is testable on a build machine with a
// mock break.  Compile the real user/heap.c against the harness, which owns a
// 256 KiB arena and checks reuse, split, merge, growth, the raw-sbrk gap, the
// end of the window and double-free behaviour.
func TestTaskWindowHeap(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Fatal(err)
	}
	cc, err := hostCC()
	if err != nil {
		t.Fatalf("%v (set TOS_CC; the host harness is part of T0)", err)
	}

	dir := t.TempDir()
	copyFile(t, filepath.Join(root, "user", "heap.c"), filepath.Join(dir, "heap.c"))
	copyFile(t, filepath.Join(root, "user", "heap.h"), filepath.Join(dir, "heap.h"))
	copyFile(t, filepath.Join(root, "internal", "build", "testdata", "heap_harness.c"),
		filepath.Join(dir, "harness.c"))

	exe := filepath.Join(dir, "harness")
	if runtime.GOOS == "windows" {
		exe += ".exe"
	}
	build := exec.Command(cc[0], append(cc[1:], "-std=c99", "-Wall", "-Wextra",
		"-I", dir, "-I", filepath.Join(root, "kernel"),
		"-Dsbrk=heap_test_sbrk",
		filepath.Join(dir, "heap.c"), filepath.Join(dir, "harness.c"),
		"-o", exe)...)
	build.Dir = dir
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("compiling the heap harness failed: %v\n%s", err, out)
	}

	cmd := exec.Command(exe)
	cmd.Dir = dir
	out, err := cmd.CombinedOutput()
	if err != nil {
		t.Fatalf("the task-window heap broke an invariant: %v\n%s", err, out)
	}
	if !strings.Contains(string(out), "ok (") {
		t.Fatalf("harness did not report success:\n%s", out)
	}
	t.Logf("%s", strings.TrimSpace(string(out)))
}
