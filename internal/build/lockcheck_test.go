package build

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
)

// Guard for the rule documented in docs/CONCURRENCY.md: a call that can
// block or allocate must not appear between an acquire and its release in
// the same file.  This is deliberately narrow - it does not parse the C, it
// only brace-tracks files and matches the handful of acquire/release spellings
// the kernel actually uses.  A wider checker would become a noise source and
// get ignored, which is worse than no checker.
//
// See docs/CONCURRENCY.md for the lock order this protects.

var (
	lockAcquireRE = regexp.MustCompile(`\b(?:plat_spin_lock_irqsave|spin_lock|spin_trylock)\s*\(\s*&([A-Za-z_][A-Za-z0-9_>:-]*)`)
	lockReleaseRE = regexp.MustCompile(`\b(?:plat_spin_unlock_irqrestore|spin_unlock)\s*\(\s*&([A-Za-z_][A-Za-z0-9_>:-]*)`)
	blockingCallRE = regexp.MustCompile(`\b(proc_wait|wait_for|sleep|kmalloc|kfree|usb_cdc\w*|usb_tx_drain|fs_[A-Za-z_]\w*|echo_drain)\s*\(`)
	// Escape hatch, at end of line: /* LOCKCHECK: allow <reason> */
	lockAllowRE = regexp.MustCompile(`/\*\s*LOCKCHECK:\s*allow\s+(\S.*?)\s*\*/\s*$`)
)

type lockFinding struct {
	file string
	line int
	call string
	lock string
}

func (f lockFinding) String() string {
	return fmt.Sprintf("%s:%d: %s inside %s", f.file, f.line, f.call, f.lock)
}

// stripLockComments blanks comments and string/char literals, preserving
// every byte position and newline so finding line numbers stay exact.
func stripLockComments(src string) string {
	b := []byte(src)
	n := len(b)
	i := 0
	mode := 0 // 0 code, 1 // comment, 2 /* */ comment, 3 string, 4 char
	for i < n {
		c := b[i]
		var d byte
		if i+1 < n {
			d = b[i+1]
		}
		switch mode {
		case 0:
			if c == '/' && d == '/' {
				b[i], b[i+1] = ' ', ' '
				mode = 1
				i += 2
				continue
			}
			if c == '/' && d == '*' {
				b[i], b[i+1] = ' ', ' '
				mode = 2
				i += 2
				continue
			}
			if c == '"' {
				b[i] = ' '
				mode = 3
				i++
				continue
			}
			if c == '\'' {
				b[i] = ' '
				mode = 4
				i++
				continue
			}
			i++
		case 1:
			if c == '\n' {
				mode = 0
			} else {
				b[i] = ' '
			}
			i++
		case 2:
			if c == '*' && d == '/' {
				b[i], b[i+1] = ' ', ' '
				mode = 0
				i += 2
				continue
			}
			if c != '\n' {
				b[i] = ' '
			}
			i++
		case 3, 4:
			q := byte('"')
			if mode == 4 {
				q = '\''
			}
			if c == '\\' {
				b[i] = ' '
				if i+1 < n && b[i+1] != '\n' {
					b[i+1] = ' '
				}
				i += 2
				continue
			}
			if c == q {
				b[i] = ' '
				mode = 0
			} else if c != '\n' {
				b[i] = ' '
			}
			i++
		}
	}
	return string(b)
}

func sortedLockNames(held map[string]int) string {
	names := make([]string, 0, len(held))
	for name := range held {
		names = append(names, name)
	}
	sort.Strings(names)
	return strings.Join(names, ",")
}

// scanLockFile walks one C file, tracking acquired locks by name.  A lock
// acquired in one file and released in another is out of scope: there is no
// such path in the kernel today, and pretending otherwise would only produce
// false positives.
func scanLockFile(path string) ([]lockFinding, error) {
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	src := strings.ReplaceAll(string(raw), "\r\n", "\n")
	code := stripLockComments(src)
	origLines := strings.Split(src, "\n")
	codeLines := strings.Split(code, "\n")

	held := make(map[string]int)
	depth := 0
	var out []lockFinding
	for i, line := range codeLines {
		ln := i + 1
		orig := strings.TrimRight(origLines[i], " \t")
		allow := lockAllowRE.MatchString(orig)

		// Check the blocking call before this line's acquire, so a line that
		// takes a lock and immediately calls through it is still caught on the
		// next line.  Release first is safe: the blocking call on the same line
		// as the unlock is outside the critical section.
		for _, m := range blockingCallRE.FindAllStringSubmatch(line, -1) {
			if len(held) > 0 && !allow {
				out = append(out, lockFinding{path, ln, m[1], sortedLockNames(held)})
			}
		}
		for _, m := range lockReleaseRE.FindAllStringSubmatch(line, -1) {
			name := m[1]
			if held[name] > 1 {
				held[name]--
			} else {
				delete(held, name)
			}
		}
		for _, m := range lockAcquireRE.FindAllStringSubmatch(line, -1) {
			held[m[1]]++
		}

		depth += strings.Count(line, "{") - strings.Count(line, "}")
		if depth <= 0 && len(held) > 0 {
			// Leave the function; a lock still in the map was acquired in a
			// different translation unit or through a wrapper.  Neither is
			// checkable here, so drop it rather than report the whole file.
			held = make(map[string]int)
		}
	}
	return out, nil
}

// TestLockCheckCatchesViolation proves the scanner actually asserts something:
// a synthetic violation must be reported with its line number, and the same
// line with a reasoned LOCKCHECK escape must be accepted.
func TestLockCheckCatchesViolation(t *testing.T) {
	dir := t.TempDir()
	bad := filepath.Join(dir, "bad.c")
	body := `void f(void) {
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    kmalloc(4);
    plat_spin_unlock_irqrestore(&proc_lock, irq);
}
`
	if err := os.WriteFile(bad, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	findings, err := scanLockFile(bad)
	if err != nil {
		t.Fatal(err)
	}
	if len(findings) != 1 || findings[0].line != 3 || findings[0].call != "kmalloc" || findings[0].lock != "proc_lock" {
		t.Fatalf("synthetic violation not reported correctly: %v", findings)
	}

	good := filepath.Join(dir, "allow.c")
	allowed := `void f(void) {
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    kmalloc(4); /* LOCKCHECK: allow synthetic test of the escape hatch */
    plat_spin_unlock_irqrestore(&proc_lock, irq);
}
`
	if err := os.WriteFile(good, []byte(allowed), 0o644); err != nil {
		t.Fatal(err)
	}
	findings, err = scanLockFile(good)
	if err != nil {
		t.Fatal(err)
	}
	if len(findings) != 0 {
		t.Fatalf("LOCKCHECK escape was not honoured: %v", findings)
	}
}

func TestNoBlockingCallInsideLock(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Fatalf("not in the source tree: %v", err)
	}
	files, err := filepath.Glob(filepath.Join(root, "kernel", "*.c"))
	if err != nil || len(files) == 0 {
		t.Fatalf("cannot list kernel sources: %v (%d files)", err, len(files))
	}
	var findings []string
	for _, f := range files {
		hits, err := scanLockFile(f)
		if err != nil {
			t.Fatal(err)
		}
		for _, h := range hits {
			// Report repository-relative paths so a failure can be pasted
			// straight into an editor or a code review.
			rel, _ := filepath.Rel(root, f)
			findings = append(findings, fmt.Sprintf("%s:%d: %s inside %s", filepath.ToSlash(rel), h.line, h.call, h.lock))
		}
	}
	if len(findings) != 0 {
		t.Fatalf("blocking calls inside locks (see docs/CONCURRENCY.md):\n%s", strings.Join(findings, "\n"))
	}
	t.Logf("lockcheck: %d kernel files, 0 blocking calls inside locks", len(files))
}
