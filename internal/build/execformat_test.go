package build

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The kernel's format dispatch is C, so this pins the shape that the A-3
// review asked for: two handler tables walked by loops, and no direct
// single-instance calls left in exec().  It cannot prove that a new format
// loads; that runtime mock test is tracked in OPEN-ITEMS 2d.
func TestExecFormatDispatchIsATable(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	src, err := os.ReadFile(filepath.Join(root, "kernel", "process.c"))
	if err != nil {
		t.Fatal(err)
	}
	text := string(src)
	for _, want := range []string{
		"static const struct exec_format *const exec_formats[]",
		"static const struct exec_image_format *const exec_image_formats[]",
		"exec_formats[i]->probe",
		"exec_image_formats[i]->probe",
		"cfmt->load",
		"ifmt->load",
	} {
		if !strings.Contains(text, want) {
			t.Errorf("process.c no longer contains %q", want)
		}
	}
	for _, bad := range []string{
		"lz4tos_exec.probe(", "lz4tos_exec.load(",
		"elf32_exec.probe(", "elf32_exec.load(",
	} {
		if strings.Contains(text, bad) {
			t.Errorf("process.c still calls %q directly; dispatch must go through the tables", bad)
		}
	}
}
