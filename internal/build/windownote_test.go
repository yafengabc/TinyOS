package build

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
)

// TestWindowNoteMalformedLengths makes the note name/descriptor lengths
// wrap.  The old parser added (namesz+3)&~3 to an int and sliced before
// proving the field was inside the note; this must return without panicking.
func TestWindowNoteMalformedLengths(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	path := filepath.Join(root, "build", "fs-stage", "bin", "stk")
	data, err := os.ReadFile(path)
	if err != nil {
		t.Skip("run 'tos build' first")
	}

	// The TOS note name is the marker; its 12-byte header precedes it.
	marker := []byte("TinyOS")
	at := bytes.Index(data, marker)
	if at < 12 {
		t.Skip("no TinyOS note in the staged ELF")
	}
	bad := append([]byte(nil), data...)
	hdr := at - 12
	for i := 0; i < 8; i++ {
		bad[hdr+i] = 0xFF
	}

	// The parser can either reject the ELF or ignore the damaged note, but
	// it must not index past the end of the note slice.
	_, _ = windowNote(bad)
}
