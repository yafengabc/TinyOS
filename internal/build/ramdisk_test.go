package build

import (
	"os"
	"path/filepath"
	"testing"
)

// files.local is where a local run stages generated artifacts (an rcc .tos for
// a QEMU smoke run, say).  It must be picked up without a rebuild of the build
// tool, and it must replace rather than duplicate a file of the same name in
// the tracked files/ tree: two /bin/x entries make the image at best confusing
// and at worst unloadable.
func TestCollectFilesLocalOverride(t *testing.T) {
	root := t.TempDir()
	write := func(rel, body string) {
		t.Helper()
		p := filepath.Join(root, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("files/readme.txt", "base")
	write("files/etc/a.conf", "a")
	write("files.local/readme.txt", "override")
	write("files.local/bin/staged", "tos")

	files, err := collectFiles(root)
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]string{}
	for _, f := range files {
		if _, dup := got[f.Name]; dup {
			t.Fatalf("duplicate entry %q", f.Name)
		}
		got[f.Name] = string(f.Data)
	}
	want := map[string]string{
		"readme.txt":   "override",
		"etc/a.conf":   "a",
		"bin/staged":   "tos",
	}
	if len(got) != len(want) {
		t.Fatalf("got %d files (%v), want %d", len(got), got, len(want))
	}
	for name, body := range want {
		if got[name] != body {
			t.Errorf("%s = %q, want %q", name, got[name], body)
		}
	}
}

// A tree with no files/ directory at all is normal (some targets carry no
// static files) and must not be an error.
func TestCollectFilesMissingTrees(t *testing.T) {
	files, err := collectFiles(t.TempDir())
	if err != nil {
		t.Fatal(err)
	}
	if len(files) != 0 {
		t.Fatalf("got %d files, want 0", len(files))
	}
}
