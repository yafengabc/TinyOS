package build

import (
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
)

/* docs/CONCURRENCY.md is the prose; this is the part that cannot rot.  A
 * deadline bug once cost 34 s of boot on the RV32 SMP image: usb_cdc_flush()
 * called usb_tx_drain(), which took g_tx_lock in the opposite order to the
 * path that took g_tx_lock and then flushed.  Reading the code would not have
 * caught it; a rule "do not block under this lock" would have.
 *
 * The scan is function-level, not region-level.  Tracking acquire/release
 * pairs by brace depth looks precise and is not: a return on an error path or
 * a single-statement if leaves the counter wrong for the rest of the file,
 * which is how a checker starts reporting the wrong lines and gets ignored.
 * "This function takes proc_lock and also calls kmalloc" is coarse, but every
 * hit is real enough to answer with a one-line reason. */

// lockRules lists, per lock, the calls that must not appear in a function
// that takes it.  docs/CONCURRENCY.md explains why: everything listed is
// either a blocking sleep or a lock that sits above this one.
var lockRules = []struct {
	lock string
	call string // regexp
	why  string
}{
	{"proc_lock", `\bproc_wait\s*\(`, "waits with the process table locked"},
	{"proc_lock", `\bwait_for\s*\(`, "waits with the process table locked"},
	{"proc_lock", `\bmsleep\s*\(`, "sleeps with the process table locked"},
	{"proc_lock", `\busb_cdc\w*\s*\(`, "USB flush takes g_tx_lock, which is below proc_lock"},
	{"proc_lock", `\bfs_\w+\s*\(`, "filesystem calls take fs_lock, which is below proc_lock"},
	{"proc_lock", `\blfs_\w+\s*\(`, "littlefs runs under fs_lock"},
	{"fs_lock", `\bproc_wait\s*\(`, "waits with the filesystem locked"},
	{"fs_lock", `\bmsleep\s*\(`, "sleeps with the filesystem locked"},
	{"fs_lock", `\busb_cdc\w*\s*\(`, "USB flush would invert fs_lock against g_tx_lock"},
	{"heap_lock", `\bkmalloc\s*\(|\bkfree\s*\(`, "the heap lock is not recursive"},
	{"heap_lock", `\bfs_\w+\s*\(`, "filesystem calls take fs_lock, which is above heap_lock"},
}

var (
	lockAcquire = regexp.MustCompile(`\b(?:plat_spin_lock_irqsave|spin_lock)\s*\(\s*&(\w+)`)
	funcStart   = regexp.MustCompile(`^[A-Za-z_][A-Za-z0-9_ \t\*]*\b([A-Za-z_][A-Za-z0-9_]*)\s*\(`)
)

var forbiddenRe = func() []*regexp.Regexp {
	out := make([]*regexp.Regexp, len(lockRules))
	for i, r := range lockRules {
		out[i] = regexp.MustCompile(r.call)
	}
	return out
}()

type cFunc struct {
	name       string
	start, end int // 1-based, inclusive
}

// kernelFunctions splits a C file into top-level function bodies.  It only
// needs to be good enough to attribute a call to a function; anything it gets
// wrong shows up as a false hit, never as a missed one.
func kernelFunctions(lines []string) []cFunc {
	var out []cFunc
	depth := 0
	start := 0
	name := ""
	for i, line := range lines {
		if depth == 0 {
			if m := funcStart.FindStringSubmatch(line); m != nil &&
				!strings.HasSuffix(strings.TrimSpace(line), ";") &&
				!strings.HasPrefix(strings.TrimSpace(line), "typedef") {
				start, name = i+1, m[1]
			}
		}
		depth += strings.Count(line, "{") - strings.Count(line, "}")
		if depth < 0 {
			depth = 0
		}
		if depth == 0 && start != 0 && i+1 > start {
			out = append(out, cFunc{name: name, start: start, end: i + 1})
			start = 0
		}
	}
	return out
}

// scanLocks returns one line per violation in a C file's text.
func scanLocks(name, text string) []string {
	lines := strings.Split(text, "\n")
	var hits []string
	for _, fn := range kernelFunctions(lines) {
		body := strings.Join(lines[fn.start-1:fn.end], "\n")
		// Start after the opening brace: the signature itself contains the
		// function's own name, and kmalloc() would otherwise "call itself".
		open := strings.Index(body, "{")
		if open < 0 {
			continue
		}
		inner := body[open+1:]
		base := fn.start + strings.Count(body[:open], "\n")
		if strings.Contains(inner, "LOCKCHECK: allow") {
			continue
		}
		var held []string
		for _, r := range lockRules {
			if strings.Contains(inner, "&"+r.lock) {
				held = append(held, r.lock)
			}
		}
		for i, r := range lockRules {
			if !containsString(held, r.lock) {
				continue
			}
			if loc := forbiddenRe[i].FindStringIndex(inner); loc != nil {
				line := base + strings.Count(inner[:loc[0]], "\n")
				hits = append(hits, name+":"+itoa(line)+": "+fn.name+
					" holds "+r.lock+" and calls "+forbiddenRe[i].FindString(inner)+
					" ("+r.why+")")
			}
		}
	}
	return hits
}

// TestLockDiscipline fails when a function that takes a lock also makes a call
// that the lock order forbids.  Escape hatch: a comment containing
// "LOCKCHECK: allow <reason>" anywhere in the function body.
func TestLockDiscipline(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Fatal(err)
	}
	files, _ := filepath.Glob(filepath.Join(root, "kernel", "*.c"))
	if len(files) == 0 {
		t.Fatal("no kernel sources")
	}
	var hits []string
	for _, path := range files {
		data, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		hits = append(hits, scanLocks(filepath.Base(path), string(data))...)
	}
	sort.Strings(hits)
	for _, h := range hits {
		t.Errorf("%s", h)
	}
	if len(hits) > 0 {
		t.Logf("%d hit(s); add a LOCKCHECK: allow <reason> in the function "+
			"if the call really does not happen while the lock is held", len(hits))
	}
}

// TestLockDisciplineDetects is the negative case: the scanner has to report a
// real violation, or TestLockDiscipline's green means nothing.
func TestLockDisciplineDetects(t *testing.T) {
	src := `
static void bad(void)
{
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    proc_wait(1);
    plat_spin_unlock_irqrestore(&proc_lock, irq);
}

static void excused(void)
{
    uint32_t irq = plat_spin_lock_irqsave(&proc_lock);
    /* LOCKCHECK: allow the wait happens after the unlock below */
    fs_mount();
    plat_spin_unlock_irqrestore(&proc_lock, irq);
}

static void innocent(void)
{
    proc_wait(2);
}
`
	hits := scanLocks("fake.c", src)
	if len(hits) != 1 {
		t.Fatalf("want exactly one hit, got %d: %v", len(hits), hits)
	}
	if !strings.Contains(hits[0], "proc_lock") || !strings.Contains(hits[0], "proc_wait") {
		t.Fatalf("hit does not name the lock and the call: %q", hits[0])
	}
	if !strings.Contains(hits[0], "bad") {
		t.Fatalf("hit does not name the function: %q", hits[0])
	}
}

func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var b []byte
	for n > 0 {
		b = append([]byte{byte('0' + n%10)}, b...)
		n /= 10
	}
	return string(b)
}
