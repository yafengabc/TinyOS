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
 * Exceptions live in testdata/include-allow.txt as "path: pattern  # reason".
 * The reasons matter: an entry without one is not an exemption.  A baseline
 * entry that no longer matches is a failure too, so a reason cannot quietly
 * outlive the code it excused. */

type includeRule struct {
	name  string
	dir   string // directory scanned, relative to root
	re    *regexp.Regexp // include paths the rule forbids
	allow bool   // whether testdata baseline entries apply
}

var includeRules = []includeRule{
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
		name:  "kernel must not include board implementation headers",
		dir:   "kernel",
		re:    regexp.MustCompile(`^#\s*include\s*"(?:\.\./)?board/`),
		allow: true,
	},
	{
		name:  "arch must not include concrete driver headers",
		dir:   "arch",
		re:    regexp.MustCompile(`^#\s*include\s*"(?:\.\./)?drivers/`),
		allow: true,
	},
}

// The one hard rule: no NEW multiword plat_* declaration in the aggregate
// platform.h.  Peripheral names are the motivating case, but a new
// plat_lcd_probe() is just as much a interface leak as plat_gpio_write().
// The declarations present when the split landed are pinned in
// testdata/platform-periph.txt; moving any of them into a narrow header
// shrinks the set and is always fine.
var platPeriphDeclRE = regexp.MustCompile(`^\s*(?:extern\s+)?(?:int|void|uint32_t)\s+plat_[a-z][a-z0-9]*_[A-Za-z0-9_]*\s*\(`)

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

// loadPlatformPeriph reads the pinned set of peripheral declarations that
// existed in platform.h when the split landed.  Blank lines and # comments
// are ignored; every other line is a bare symbol name.
func loadPlatformPeriph(path string) (map[string]bool, error) {
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

func scanIncludeRule(root string, rule includeRule, allows map[string][]*allowEntry) ([]includeHit, error) {
	var files []string
	err := filepath.Walk(filepath.Join(root, rule.dir), func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if !info.IsDir() && strings.HasSuffix(path, ".c") {
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
			if !rule.re.MatchString(line) {
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

	var problems []string
	for _, rule := range includeRules {
		hits, err := scanIncludeRule(root, rule, allows)
		if err != nil {
			t.Fatal(err)
		}
		for _, h := range hits {
			problems = append(problems, "new violation: "+h.String())
		}
	}

	// The one non-file rule: platform.h is the aggregate, new peripheral
	// declarations must go to a narrow header instead.  Existing ones are
	// pinned by testdata/platform-periph.txt so "no new" is checkable.
	ph := filepath.Join(root, "kernel", "platform.h")
	data, err := os.ReadFile(ph)
	if err != nil {
		t.Fatal(err)
	}
	existing, err := loadPlatformPeriph(filepath.Join(root, "internal", "build", "testdata", "platform-periph.txt"))
	if err != nil {
		t.Fatal(err)
	}
	periphRE := regexp.MustCompile(`plat_[a-z][a-z0-9]*_[A-Za-z0-9_]*`)
	for i, line := range strings.Split(string(data), "\n") {
		if !platPeriphDeclRE.MatchString(line) {
			continue
		}
		name := periphRE.FindString(line)
		if !existing[name] {
			problems = append(problems, fmt.Sprintf("kernel/platform.h:%d: new peripheral declaration %s: %s", i+1, name, strings.TrimSpace(line)))
		}
	}

	// A baseline entry that no longer matches is a failure: reasons must not
	// outlive the code they excused.
	var unused []string
	for file, entries := range allows {
		for _, a := range entries {
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
	t.Logf("include directions: %d rules, %d baseline entries, 0 new violations", len(includeRules), len(allows))
}
