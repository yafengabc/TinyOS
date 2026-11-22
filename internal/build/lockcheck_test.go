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

// Guard for the rule documented in docs/CONCURRENCY.md: a call that can block
// or allocate must not appear between an acquire and its release.
//
// Scope - read this before extending it.  The scan is per file and per
// function body, and only sees *direct* calls: a lock taken in one function
// and a blocking call in a callee is invisible.  The 34 s RV32 SMP reboot
// (HISTORY.md:34) was exactly that shape (usb_cdc_flush -> usb_tx_drain ->
// g_tx_lock, inverted by the other path), so it would NOT have been caught.
// The value here is the cheap, local case: a new kmalloc or wait slipped into
// a critical section that is already in front of you.  Cross-function nesting
// and the driver-side USB order stay with T2/T3.
//
// The forbidden set is per lock: kmalloc under fs_lock is the documented
// fs_lock -> heap_lock order, not a bug.

var (
	lockAcquireRE = regexp.MustCompile(`\b(?:plat_spin_lock_irqsave|spin_lock)\s*\(\s*&([A-Za-z_][A-Za-z0-9_>:-]*)`)
	lockReleaseRE = regexp.MustCompile(`\b(?:plat_spin_unlock_irqrestore|spin_unlock)\s*\(\s*&([A-Za-z_][A-Za-z0-9_>:-]*)`)
	// Escape hatch, at end of line: /* LOCKCHECK: allow <reason> */
	lockAllowRE = regexp.MustCompile(`/\*\s*LOCKCHECK:\s*allow\s+(\S.*?)\s*\*/\s*$`)

	// spin_trylock is deliberately absent from lockAcquireRE: a failed
	// trylock holds nothing, so treating it as acquired would report the
	// console's "try, otherwise drop the block" paths as violations.
)

type forbiddenCall struct {
	re  *regexp.Regexp
	why string
}

func forbidden(pattern, why string) forbiddenCall {
	return forbiddenCall{regexp.MustCompile(pattern), why}
}

// lockForbids is the executable form of the "禁止事项" section.  A lock that
// is not listed here is a leaf with no call restrictions.
var lockForbids = map[string][]forbiddenCall{
	"proc_lock": {
		forbidden(`\bproc_wait\s*\(`, "sleeps with the process table locked"),
		forbidden(`\bwait_for\s*\(`, "sleeps with the process table locked"),
		forbidden(`\bmsleep\s*\(`, "sleeps with the process table locked"),
		forbidden(`\bkmalloc\s*\(|\bkfree\s*\(`, "the heap lock is not below proc_lock; allocate before taking it"),
		forbidden(`\busb_cdc\w*\s*\(|\busb_tx_drain\s*\(`, "USB flush takes g_tx_lock, which must not be taken under proc_lock"),
		forbidden(`\bfs_[A-Za-z_]\w*\s*\(`, "filesystem calls take fs_lock; do IO before taking proc_lock"),
		forbidden(`\blfs_[A-Za-z_]\w*\s*\(`, "littlefs runs under fs_lock"),
		forbidden(`\becho_drain\s*\(`, "waits on the console"),
	},
	"fs_lock": {
		forbidden(`\bproc_wait\s*\(`, "sleeps with the filesystem locked"),
		forbidden(`\bwait_for\s*\(`, "sleeps with the filesystem locked"),
		forbidden(`\bmsleep\s*\(`, "sleeps with the filesystem locked"),
		forbidden(`\busb_cdc\w*\s*\(|\busb_tx_drain\s*\(`, "USB flush must not be nested under fs_lock"),
	},
	"heap_lock": {
		forbidden(`\bkmalloc\s*\(|\bkfree\s*\(`, "the heap lock is not recursive"),
		forbidden(`\bfs_[A-Za-z_]\w*\s*\(`, "fs_lock is above heap_lock"),
		forbidden(`\bproc_wait\s*\(|\bwait_for\s*\(|\bmsleep\s*\(`, "never sleep under the heap lock"),
	},
}

type lockFinding struct {
	file string
	line int
	call string
	lock string
	why  string
}

func (f lockFinding) String() string {
	return fmt.Sprintf("%s:%d: %s inside %s (%s)", f.file, f.line, f.call, f.lock, f.why)
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

// lockEvent is one acquire, release or candidate blocking call on a line,
// ordered by column so "unlock, then call" on one line is not a false hit and
// "lock, then call" on one line is not a miss.
type lockEvent struct {
	col  int
	kind int // 0 acquire, 1 release, 2 call
	name string
	rule forbiddenCall // for calls: which rule matched
}

func lineLockEvents(line string) []lockEvent {
	var evs []lockEvent
	for _, m := range lockAcquireRE.FindAllStringSubmatchIndex(line, -1) {
		evs = append(evs, lockEvent{col: m[0], kind: 0, name: line[m[2]:m[3]]})
	}
	for _, m := range lockReleaseRE.FindAllStringSubmatchIndex(line, -1) {
		evs = append(evs, lockEvent{col: m[0], kind: 1, name: line[m[2]:m[3]]})
	}
	for _, calls := range lockForbids {
		for _, c := range calls {
			for _, m := range c.re.FindAllStringSubmatchIndex(line, -1) {
				name := strings.TrimSpace(line[m[0]:m[1]])
				name = strings.TrimSuffix(name, "(")
				evs = append(evs, lockEvent{col: m[0], kind: 2,
					name: name, rule: c})
			}
		}
	}
	sort.SliceStable(evs, func(i, j int) bool { return evs[i].col < evs[j].col })
	return evs
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
		allow := lockAllowRE.MatchString(strings.TrimRight(origLines[i], " \t"))

		for _, ev := range lineLockEvents(line) {
			switch ev.kind {
			case 0:
				held[ev.name]++
			case 1:
				if held[ev.name] > 1 {
					held[ev.name]--
				} else {
					delete(held, ev.name)
				}
			case 2:
				if allow {
					continue
				}
				// Report every held lock that forbids this call.  Sorted
				// names keep the failure output stable between runs.
				var names []string
				for name := range held {
					names = append(names, name)
				}
				sort.Strings(names)
				for _, name := range names {
					for _, c := range lockForbids[name] {
						if c != ev.rule {
							continue
						}
						out = append(out, lockFinding{path, ln, ev.name, name, c.why})
					}
				}
			}
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

// TestLockCheckLineOrder covers the one-line shapes: an acquire followed by a
// call is a violation, an unlock followed by a call is not, and a trylock is
// not an acquire (the console drops the block when it loses).
func TestLockCheckLineOrder(t *testing.T) {
	dir := t.TempDir()
	write := func(name, body string) string {
		p := filepath.Join(dir, name)
		if err := os.WriteFile(p, []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
		return p
	}
	cases := []struct {
		name string
		body string
		want int
	}{
		{"same line lock then call", `void f(void) {
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock); kmalloc(4);
    plat_spin_unlock_irqrestore(&proc_lock, irq);
}
`, 1},
		{"same line unlock then call", `void f(void) {
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    plat_spin_unlock_irqrestore(&proc_lock, irq); kmalloc(4);
}
`, 0},
		{"trylock is conditional", `void f(void) {
    if (spin_trylock(&out_lock)) kmalloc(4);
}
`, 0},
		{"kmalloc under fs_lock is the documented order", `void f(void) {
    spin_lock(&fs_lock);
    kmalloc(16);
    spin_unlock(&fs_lock);
}
`, 0},
		{"fs under heap is an inversion", `void f(void) {
    spin_lock(&heap_lock);
    fs_mount("x");
    spin_unlock(&heap_lock);
}
`, 1},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			findings, err := scanLockFile(write(c.name+".c", c.body))
			if err != nil {
				t.Fatal(err)
			}
			if len(findings) != c.want {
				t.Fatalf("want %d findings, got %v", c.want, findings)
			}
		})
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
			findings = append(findings, fmt.Sprintf("%s:%d: %s inside %s (%s)",
				filepath.ToSlash(rel), h.line, h.call, h.lock, h.why))
		}
	}
	if len(findings) != 0 {
		t.Fatalf("blocking calls inside locks (see docs/CONCURRENCY.md):\n%s", strings.Join(findings, "\n"))
	}
	t.Logf("lockcheck: %d kernel files, 0 blocking calls inside the locks it covers", len(files))
}
