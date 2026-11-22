package build

import (
	"bufio"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"testing"
)

/* include direction rules for the platform seam (issue #52).
 *
 * The platform.h split exposed a second problem: any driver could include
 * process.h and reach the scheduler, and nothing stopped a new plat_gpio_*
 * declaration from being added to the aggregate header.  These rules are
 * deliberately narrow - they check include direction and one declaration
 * pattern, not a full dependency graph.  A wide rule that fires on existing
 * code becomes a file nobody reads.
 *
 * Two of the four rules used to match "board/..." and "drivers/..." path
 * prefixes.  Nothing in the tree writes those: the build puts the board and
 * arch directories on the include path, so a leak would be a bare
 * `#include "rp2350rv_memmap.h"`.  They are now name-pool rules: the names
 * of the headers that live on the far side of the seam, minus the ones the
 * nearer directory shadows.
 *
 * Exceptions live in testdata/include-allow.txt as "path: pattern  # reason".
 * The reasons matter: an entry without one is not an exemption.  A baseline
 * entry that no longer matches is a failure too, so a reason cannot quietly
 * outlive the code it excused. */

type includeRule struct {
	name string
	dir  string
	// re matches a path-style include (the driver rules, where the header
	// names are stable).
	re *regexp.Regexp
	// pool holds bare header names that must not be included from dir.
	pool  map[string]bool
	allow bool // whether testdata baseline entries apply
}

// headerPool collects the basenames of every .h under dir, minus names that
// exist in shadowDirs: the include path resolves those to the nearer copy, so
// including the name does not reach the far side of the seam.
func headerPool(root, dir string, shadowDirs ...string) (map[string]bool, error) {
	out := map[string]bool{}
	err := filepath.Walk(filepath.Join(root, dir), func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if !info.IsDir() && strings.HasSuffix(path, ".h") {
			out[filepath.Base(path)] = true
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	for _, sd := range shadowDirs {
		_ = filepath.Walk(filepath.Join(root, sd), func(path string, info os.FileInfo, err error) error {
			if err != nil {
				return err
			}
			if !info.IsDir() && strings.HasSuffix(path, ".h") {
				delete(out, filepath.Base(path))
			}
			return nil
		})
	}
	return out, nil
}

func driverIncludeRules(root string) ([]includeRule, error) {
	board, err := headerPool(root, "board", "kernel", "include")
	if err != nil {
		return nil, err
	}
	drivers, err := headerPool(root, "drivers", "kernel", "arch", "include")
	if err != nil {
		return nil, err
	}
	return []includeRule{
		{
			name:  "drivers must not include kernel-private headers",
			dir:   "drivers",
			re:    regexp.MustCompile(`^#\s*include\s*"(process|sched[^"]*|pipe|tty|heap)\.h"`),
			allow: true,
		},
		{
			name:  "drivers must not include arch memory-map headers",
			dir:   "drivers",
			re:    regexp.MustCompile(`^#\s*include\s*"(arm_memmap|rp2350rv_memmap|scs)\.h"`),
			allow: true,
		},
		{
			name:  "kernel must not include board-only headers",
			dir:   "kernel",
			pool:  board,
			allow: true,
		},
		{
			name:  "arch must not include driver-only headers",
			dir:   "arch",
			pool:  drivers,
			allow: true,
		},
	}, nil
}

// platform-periph.txt pins every plat_* declaration the aggregate header still
// holds (not only the multiword ones), so any new one is a failure.  A pinned
// name that is gone is a failure too: the list must shrink when a declaration
// moves into a narrow header.
var platDeclRE = regexp.MustCompile(`(?m)^\s*(?:extern\s+)?(?:[A-Za-z_][A-Za-z0-9_]*[\s\*]+)+\*?(plat_[a-z0-9_]+)\s*\(`)

// platformDecls returns the plat_* names declared in a header, ignoring
// comments (which mention many of them in prose).
func platformDecls(text string) map[string]bool {
	out := map[string]bool{}
	for _, m := range platDeclRE.FindAllStringSubmatch(stripLockComments(text), -1) {
		out[m[1]] = true
	}
	return out
}

type includeHit struct {
	rule string
	file string
	line int
	text string
}

func (h includeHit) String() string {
	return fmt.Sprintf("%s:%d: %s: %s", h.file, h.line, h.rule, strings.TrimSpace(h.text))
}

type allowEntry struct {
	file    string
	pattern string
	reason  string
	used    bool
}

func loadIncludeAllow(path string) (map[string][]*allowEntry, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	out := map[string][]*allowEntry{}
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		// path: pattern  # reason
		// The pattern contains "#include", so find the comment marker after
		// the pattern, not the first '#'.  The format requires two spaces
		// before the marker.
		i := strings.Index(line, ":")
		j := strings.Index(line, "  #")
		if i < 0 || j < 0 || j <= i+1 {
			return nil, fmt.Errorf("%s: want 'path: pattern  # reason', got %q", path, line)
		}
		file := strings.TrimSpace(line[:i])
		pattern := strings.TrimSpace(line[i+1 : j])
		reason := strings.TrimSpace(line[j+3:])
		if reason == "" {
			return nil, fmt.Errorf("%s: entry for %s has no reason", path, file)
		}
		out[filepath.ToSlash(file)] = append(out[filepath.ToSlash(file)], &allowEntry{file: file, pattern: pattern, reason: reason})
	}
	return out, sc.Err()
}

// loadPinnedDecls reads the pinned set of plat_* declarations left in the
// aggregate header.  Blank lines and # comments are ignored; every other line
// is a bare symbol name.
func loadPinnedDecls(path string) (map[string]bool, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	out := map[string]bool{}
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		out[line] = true
	}
	return out, nil
}

func allowedHit(allows map[string][]*allowEntry, hit includeHit) (*allowEntry, bool) {
	for _, a := range allows[hit.file] {
		if strings.Contains(hit.text, a.pattern) {
			a.used = true
			return a, true
		}
	}
	return nil, false
}

var includeLineRE = regexp.MustCompile(`^#\s*include\s*"([^"]+)"`)

func scanIncludeRule(root string, rule includeRule, allows map[string][]*allowEntry) ([]includeHit, error) {
	var files []string
	err := filepath.Walk(filepath.Join(root, rule.dir), func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		// Headers leak too: a drivers/*.h that includes process.h hands the
		// scheduler to every .c that includes it.
		if !info.IsDir() && (strings.HasSuffix(path, ".c") || strings.HasSuffix(path, ".h")) {
			files = append(files, path)
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	var hits []includeHit
	for _, path := range files {
		rel, _ := filepath.Rel(root, path)
		rel = filepath.ToSlash(rel)
		data, err := os.ReadFile(path)
		if err != nil {
			return nil, err
		}
		for i, line := range strings.Split(string(data), "\n") {
			match := rule.re != nil && rule.re.MatchString(line)
			if !match && rule.pool != nil {
				if m := includeLineRE.FindStringSubmatch(line); m != nil {
					match = rule.pool[filepath.Base(m[1])]
				}
			}
			if !match {
				continue
			}
			h := includeHit{rule.name, rel, i + 1, line}
			if a, ok := allowedHit(allows, h); ok {
				h.text += "  /* allow: " + a.reason + " */"
				continue
			}
			hits = append(hits, h)
		}
	}
	return hits, nil
}

func TestIncludeDirections(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Fatal(err)
	}
	allowPath := filepath.Join(root, "internal", "build", "testdata", "include-allow.txt")
	allows, err := loadIncludeAllow(allowPath)
	if err != nil {
		t.Fatal(err)
	}
	rules, err := driverIncludeRules(root)
	if err != nil {
		t.Fatal(err)
	}

	var problems []string
	for _, rule := range rules {
		hits, err := scanIncludeRule(root, rule, allows)
		if err != nil {
			t.Fatal(err)
		}
		for _, h := range hits {
			problems = append(problems, "new violation: "+h.String())
		}
	}

	// platform.h is the aggregate: every plat_* declaration still there is
	// pinned, so a new one fails whether it is multiword, a different return
	// type, or static inline.  A pinned name that disappeared must be removed
	// from the file, or the list stops describing the header.
	ph := filepath.Join(root, "kernel", "platform.h")
	data, err := os.ReadFile(ph)
	if err != nil {
		t.Fatal(err)
	}
	pinned, err := loadPinnedDecls(filepath.Join(root, "internal", "build", "testdata", "platform-periph.txt"))
	if err != nil {
		t.Fatal(err)
	}
	decls := platformDecls(string(data))
	var added, stale []string
	for name := range decls {
		if !pinned[name] {
			added = append(added, name)
		}
	}
	for name := range pinned {
		if !decls[name] {
			stale = append(stale, name)
		}
	}
	sort.Strings(added)
	sort.Strings(stale)
	for _, name := range added {
		problems = append(problems, "kernel/platform.h: new plat_* declaration "+name+" (move it to kernel/plat/)")
	}
	for _, name := range stale {
		problems = append(problems, "testdata/platform-periph.txt: "+name+" is pinned but no longer declared in platform.h")
	}

	// A baseline entry that no longer matches is a failure: reasons must not
	// outlive the code they excused.
	var unused []string
	entries := 0
	for file, list := range allows {
		for _, a := range list {
			entries++
			if !a.used {
				unused = append(unused, fmt.Sprintf("%s: %q (reason: %s)", file, a.pattern, a.reason))
			}
		}
	}
	sort.Strings(unused)
	for _, u := range unused {
		problems = append(problems, "stale baseline entry: "+u)
	}

	if len(problems) != 0 {
		t.Fatalf("include direction problems:\n%s", strings.Join(problems, "\n"))
	}
	t.Logf("include directions: %d rules, %d baseline entries in %d files, "+
		"%d pinned declarations, 0 new violations",
		len(rules), entries, len(allows), len(pinned))
}

// TestIncludeRulesDetect is the negative case: a bare include on the wrong
// side of the seam must be reported.  Without it, "0 new violations" could
// just be a rule that matches nothing - which is exactly what the path-prefix
// version did.
func TestIncludeRulesDetect(t *testing.T) {
	dir := t.TempDir()
	write := func(rel, body string) {
		t.Helper()
		p := filepath.Join(dir, filepath.FromSlash(rel))
		if err := os.MkdirAll(filepath.Dir(p), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(p, []byte(body), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	write("board/rp2350rv-generic/rp2350rv_memmap.h", "/* board */\n")
	write("board/rp2350rv-generic/board.h", "/* board */\n")
	write("kernel/board.h", "/* kernel copy shadows it */\n")
	write("drivers/usb/usb_cdc.h", "/* driver */\n")
	write("arch/arm/driver_shim.h", "/* arch copy shadows it */\n")
	write("kernel/leak.c", "#include \"rp2350rv_memmap.h\"\n#include \"board.h\"\n")
	write("kernel/headers.h", "#include \"rp2350rv_memmap.h\"\n")
	write("arch/arm/leak.c", "#include \"usb_cdc.h\"\n")

	rules, err := driverIncludeRules(dir)
	if err != nil {
		t.Fatal(err)
	}
	var hits []string
	for _, rule := range rules {
		got, err := scanIncludeRule(dir, rule, map[string][]*allowEntry{})
		if err != nil {
			t.Fatal(err)
		}
		for _, h := range got {
			hits = append(hits, h.file+":"+fmt.Sprint(h.line))
		}
	}
	sort.Strings(hits)
	want := []string{"arch/arm/leak.c:1", "kernel/headers.h:1", "kernel/leak.c:1"}
	if len(hits) != len(want) || strings.Join(hits, ",") != strings.Join(want, ",") {
		t.Fatalf("want %v, got %v", want, hits)
	}
}

// TestPlatformDeclsDetect covers both directions of the pin: a new
// declaration fails, and a pinned one that no longer exists fails.
func TestPlatformDeclsDetect(t *testing.T) {
	text := `
/* comment mentions plat_fake_thing() and must not count */
void plat_init(void);
int plat_gpio_new(void);
static inline uint32_t plat_timer(void) { return 0; }
const char *plat_name(void);
`
	got := platformDecls(text)
	for _, want := range []string{"plat_init", "plat_gpio_new", "plat_timer", "plat_name"} {
		if !got[want] {
			t.Errorf("declaration %s not found in %v", want, got)
		}
	}
	if got["plat_fake_thing"] {
		t.Errorf("comment text was treated as a declaration")
	}
	pinned := map[string]bool{"plat_init": true, "plat_gone": true}
	var added, stale []string
	for name := range got {
		if !pinned[name] {
			added = append(added, name)
		}
	}
	for name := range pinned {
		if !got[name] {
			stale = append(stale, name)
		}
	}
	sort.Strings(added)
	if strings.Join(added, ",") != "plat_gpio_new,plat_name,plat_timer" {
		t.Errorf("added = %v", added)
	}
	if strings.Join(stale, ",") != "plat_gone" {
		t.Errorf("stale = %v", stale)
	}
}
