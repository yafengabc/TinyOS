package build

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// The flash-park handshake only ever runs with XIP off, so its two known bugs
// (ack published once per entry, and acking the wrong generation) were each
// found by a board wedging on a flash write.  The state machine is plain C, so
// the rules can be exercised on a desk between board sessions.
//
// This compiles board/rp_park.h with a host compiler and runs the model in
// testdata/rppark_model.c: one handshake, a stale ack, two requests back to
// back while the parked core is still in its loop, and a hand-written stale
// ack value.  It does not replace the 1000-round soak - it catches the class of
// mistake that soak exists to find, before the board is busy.
func TestRpParkModel(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	cc, err := hostCC()
	if err != nil {
		t.Skip(err)
	}

	dir := t.TempDir()
	copyFile(t, filepath.Join(root, "board", "rp_park.h"), filepath.Join(dir, "rp_park.h"))
	copyFile(t, filepath.Join(root, "internal", "build", "testdata", "rppark_model.c"),
		filepath.Join(dir, "model.c"))
	// rp_park.h includes "types.h" for the fixed-width types only.
	if err := os.WriteFile(filepath.Join(dir, "types.h"), []byte("#include <stdint.h>\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	exe := filepath.Join(dir, "model")
	if runtime.GOOS == "windows" {
		exe += ".exe"
	}
	build := exec.Command(cc[0], append(cc[1:], "-std=c99", "-Wall", "-Wextra", "-I", dir,
		filepath.Join(dir, "model.c"), "-o", exe)...)
	build.Dir = dir
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("compiling the park model failed: %v\n%s", err, out)
	}

	run := exec.Command(exe)
	run.Dir = dir
	out, err := run.CombinedOutput()
	text := string(out)
	if err != nil {
		t.Fatalf("park model failed: %v\n%s", err, text)
	}
	if !strings.Contains(text, "MODEL OK") {
		t.Fatalf("park model did not report success:\n%s", text)
	}
	// Keep the transcript available with -v: it is the evidence, and it names
	// which rule broke when one does.
	t.Logf("\n%s", text)
}
