package build

import (
	"encoding/hex"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// TestCorpus runs the committed malformed inputs in tests/corpus through the
// parsers the build uses.  The point is not to cover every branch: it is to
// pin "accept / reject / never panic" for the same bytes that the C side (and
// goc32, later) will read, so the three implementations cannot drift apart
// silently.
//
// No Skip paths: a missing corpus directory is a failure, because a test that
// quietly does nothing is how the old fixtures (corrupt_test.go,
// windownote_test.go) ended up guarding nothing.
func TestCorpus(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Fatal(err)
	}
	base := filepath.Join(root, "tests", "corpus")
	manifest, err := os.ReadFile(filepath.Join(base, "cases.txt"))
	if err != nil {
		t.Fatalf("tests/corpus/cases.txt: %v (run python tests/corpus/gen.py)", err)
	}
	var cases []string
	for _, line := range strings.Split(string(manifest), "\n") {
		if line = strings.TrimSpace(line); line != "" {
			cases = append(cases, line)
		}
	}
	if len(cases) < 10 {
		t.Fatalf("only %d corpus cases; the manifest should list them all", len(cases))
	}

	// Every case on disk must be in the manifest, so a stale directory cannot
	// look covered.
	onDisk := 0
	if err := filepath.Walk(base, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.Name() == "input.bin" {
			onDisk++
		}
		return nil
	}); err != nil {
		t.Fatal(err)
	}
	if onDisk != len(cases) {
		t.Fatalf("%d input.bin on disk but %d in cases.txt", onDisk, len(cases))
	}

	// One binary, built from the real kernel/lz4.c, so the LZ4 cases are
	// checked against both implementations.  A missing compiler fails the
	// test: the corpus is the guard, and a guard that skips is not one.
	tool := buildLZ4CorpusHarness(t, root)

	for _, c := range cases {
		c := c
		t.Run(c, func(t *testing.T) {
			data, err := os.ReadFile(filepath.Join(base, c, "input.bin"))
			if err != nil {
				t.Fatal(err)
			}
			raw, err := os.ReadFile(filepath.Join(base, c, "expect"))
			if err != nil {
				t.Fatal(err)
			}
			want := strings.TrimSpace(string(raw))

			// A panic is a bug whatever the input says: report it as the
			// case's failure instead of taking the whole run down.
			defer func() {
				if r := recover(); r != nil {
					t.Fatalf("panic on %s: %v", c, r)
				}
			}()

			var got string
			switch {
			case strings.HasPrefix(c, "elf/"):
				w, ok := windowNote(data)
				if !ok {
					got = "reject"
				} else {
					got = fmt.Sprintf("accept stack=%d heap=%d window=%d span=%#x",
						w.stack, w.heap, w.window, w.span)
				}
			case strings.HasPrefix(c, "lz4/"):
				out, derr := lz4DecompressBlock(data, 4096)
				if derr != nil {
					got = "reject"
				} else {
					got = "accept " + hex.EncodeToString(out)
				}
				// The kernel decoder has to agree with the Go port on the same
				// bytes.  A panic guard above; a disagreement is a corpus case
				// that found a real difference.
				cout, cerr := exec.Command(tool, filepath.Join(base, c, "input.bin")).Output()
				if cerr != nil {
					t.Fatalf("%s: C harness: %v", c, cerr)
				}
				if cgot := strings.TrimSpace(string(cout)); cgot != strings.TrimSpace(got) {
					t.Fatalf("%s: Go says %q, kernel decoder says %q", c, got, cgot)
				}
			default:
				t.Fatalf("no parser for corpus family %q", c)
			}
			if strings.TrimSpace(got) != want {
				t.Fatalf("%s\n got: %s\nwant: %s", c, got, want)
			}
		})
	}
}

// buildLZ4CorpusHarness compiles testdata/corpus_lz4_harness.c against the
// real kernel sources.  It fails rather than skips when there is no host
// compiler: on a build machine the corpus is part of T0.
func buildLZ4CorpusHarness(t *testing.T, root string) string {
	t.Helper()
	cc, err := hostCC()
	if err != nil {
		t.Fatalf("%v (the corpus needs a host C compiler; set TOS_CC)", err)
	}
	dir := t.TempDir()
	for _, src := range []string{
		filepath.Join(root, "kernel", "lz4.c"),
		filepath.Join(root, "internal", "build", "testdata", "corpus_lz4_harness.c"),
	} {
		copyFile(t, src, filepath.Join(dir, filepath.Base(src)))
	}
	copyFile(t, filepath.Join(root, "kernel", "lz4.h"), filepath.Join(dir, "lz4.h"))
	copyFile(t, filepath.Join(root, "kernel", "string.h"), filepath.Join(dir, "string.h"))
	// The kernel's types.h describes a 32-bit target; on the host it collides
	// with the libc headers the harness needs (size_t is 64-bit there).
	// Shim it, the same way the LCD mock does, and let libc provide
	// memcpy/memset - the decoder does not depend on the kernel's
	// word-at-a-time implementation, only on the prototypes in string.h.
	if err := os.WriteFile(filepath.Join(dir, "types.h"),
		[]byte("#include <stdint.h>\n#include <stddef.h>\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	exe := filepath.Join(dir, "corpus_lz4")
	if runtime.GOOS == "windows" {
		exe += ".exe"
	}
	args := append(cc[1:], "-std=c99", "-Wall", "-Wextra", "-I", dir,
		filepath.Join(dir, "lz4.c"),
		filepath.Join(dir, "corpus_lz4_harness.c"), "-o", exe)
	build := exec.Command(cc[0], args...)
	build.Dir = dir
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("compiling the LZ4 corpus harness failed: %v\n%s", err, out)
	}
	return exe
}
