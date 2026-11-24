package build

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

/* Host tests for the pure kernel logic.  The pattern is heap_test.go's: the
 * Go test compiles the real source against a harness in testdata/, so there is
 * no second copy of the code and no board in the loop.
 *
 * Unlike heap_test.go these must not skip.  On a machine without a host
 * compiler T0 fails, because a guard that silently does nothing is the thing
 * this work was meant to remove: the old fixtures skipped three different ways
 * each and covered nothing on a clean checkout. */

type hostHarness struct {
	name string   // harness .c file in testdata/
	srcs []string // real sources, relative to the repository root
	hdrs []string // headers the sources include
	shim map[string]string
}

var hostHarnesses = []hostHarness{
	{
		name: "uaccess_harness.c",
		srcs: []string{"kernel/uaccess.c"},
		hdrs: []string{"kernel/uaccess.h"},
		shim: map[string]string{"types.h": "#include <stdint.h>\n#include <stddef.h>\n"},
	},
	{
		name: "u32math_harness.c",
		// Header-only: nothing to link, the harness includes it directly.
		hdrs: []string{"kernel/u32math.h"},
		shim: map[string]string{"types.h": "#include <stdint.h>\n#include <stddef.h>\n"},
	},
	{
		// The panel text layer: tab expansion, wrapping, scrolling and the
		// last-column TAB that lived forever on the ATK board (5260d28).
		// The harness's stub panel counts cells and exits on a runaway, so
		// that regression fails instead of hanging T0.
		name: "screen_harness.c",
		srcs: []string{"kernel/screen.c"},
		shim: map[string]string{
			"types.h": "#include <stdint.h>\n#include <stddef.h>\n",
			"cpu.h":   "typedef int spin_t;\n#define SPIN_INIT 0\n" +
				"void spin_lock(spin_t *s);\nvoid spin_unlock(spin_t *s);\n",
			// screen.c reaches the panel only through these (kernel/hwdev.h);
			// the harness implements them.
			"hwdev.h": "#include <stdint.h>\n" +
				"int plat_display_count(void);\n" +
				"int plat_display_info(int idx, int *w, int *h, int *cw, int *ch);\n" +
				"int plat_display_cell(int idx, int col, int row, char c, uint16_t fg, uint16_t bg);\n" +
				"int plat_display_row(int idx, int row, const char *chars, int ncols, uint16_t fg, uint16_t bg);\n" +
				"int plat_display_clear(int idx, uint16_t color);\n" +
				"int plat_display_backlight(int idx, int on);\n" +
				"int plat_display_backlight_get(int idx);\n" +
				"void plat_display_sync(int idx);\n",
			"console.h":  "",
			"platform.h": "",
			"string.h": "#include <stddef.h>\n" +
				"void *memmove(void *d, const void *s, size_t n);\n" +
				"void *memcpy(void *d, const void *s, size_t n);\n" +
				"void *memset(void *d, int c, size_t n);\n",
		},
	},
	{
		name: "pipe_harness.c",
		srcs: []string{"kernel/pipe.c"},
		hdrs: []string{"kernel/pipe.h"},
		shim: map[string]string{
			"types.h":    "#include <stdint.h>\n#include <stddef.h>\n",
			"cpu.h":      "typedef int spin_t;\n",
			"heap.h":     "void *kmalloc(unsigned int n);\nvoid kfree(void *p);\n",
			"platform.h": "uint32_t plat_spin_lock_irqsave(spin_t *s);\nvoid plat_spin_unlock_irqrestore(spin_t *s, uint32_t irq);\n",
			"string.h":   "#include <stddef.h>\nvoid *memset(void *d, int c, size_t n);\nint memcmp(const void *a, const void *b, size_t n);\n",
		},
	},
}

func TestHostHarnesses(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Fatal(err)
	}
	for _, h := range hostHarnesses {
		h := h
		t.Run(strings.TrimSuffix(h.name, "_harness.c"), func(t *testing.T) {
			exe := buildHostHarness(t, root, h)
			out, err := exec.Command(exe).CombinedOutput()
			if err != nil {
				t.Fatalf("%v\n%s", err, out)
			}
			want := strings.TrimSuffix(h.name, "_harness.c") + " ok"
			if !strings.Contains(string(out), want) {
				t.Fatalf("want %q in the output, got:\n%s", want, out)
			}
		})
	}
}

func buildHostHarness(t *testing.T, root string, h hostHarness) string {
	t.Helper()
	cc, err := hostCC()
	if err != nil {
		t.Fatalf("%v (set TOS_CC; the host harnesses are part of T0)", err)
	}
	dir := t.TempDir()
	copyFile(t, filepath.Join(root, "internal", "build", "testdata", h.name),
		filepath.Join(dir, h.name))
	for _, rel := range append(append([]string{}, h.srcs...), h.hdrs...) {
		copyFile(t, filepath.Join(root, filepath.FromSlash(rel)),
			filepath.Join(dir, filepath.Base(rel)))
	}
	for name, body := range h.shim {
		if err := os.WriteFile(filepath.Join(dir, name), []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	exe := filepath.Join(dir, "harness")
	if runtime.GOOS == "windows" {
		exe += ".exe"
	}
	args := append(cc[1:], "-std=c99", "-Wall", "-Wextra", "-I", dir)
	for _, rel := range h.srcs {
		args = append(args, filepath.Join(dir, filepath.Base(rel)))
	}
	args = append(args, filepath.Join(dir, h.name), "-o", exe)
	cmd := exec.Command(cc[0], args...)
	cmd.Dir = dir
	if out, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("compiling %s failed: %v\n%s", h.name, err, out)
	}
	return exe
}
