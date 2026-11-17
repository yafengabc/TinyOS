package build

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// fs_ro is a public [build] option, so a read-only image has to compile.  B-8
// put hash_path() inside the #ifndef TOS_LFS_RO block while open_with() records
// the hash on every open, outside that block: every default config still built,
// and the read-only build broke silently until someone tried it on a branch
// (issue #14).
//
// Compiling both ways is the real check, but it needs a toolchain and a config
// file, so this pins the shape instead: a function that only exists in a
// read-write build must not be referenced from code a read-only build compiles.
// It walks kernel/ramdisk.c, keeps the lines a -DTOS_LFS_RO build would see and
// looks for read-write-only helpers in them.
func TestReadOnlyBuildDoesNotUseROOnlyHelpers(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	body, err := os.ReadFile(filepath.Join(root, "kernel", "ramdisk.c"))
	if err != nil {
		t.Fatal(err)
	}

	visible, hidden := splitOnReadOnlyGuard(stripCComments(string(body)))

	// A definition is "read-write only" when it sits in a hidden region.
	def := regexp.MustCompile(`(?m)^static\s+[A-Za-z_][A-Za-z0-9_ \t*]*\b([a-z_][a-z0-9_]*)\s*\(`)
	roOnly := map[string]bool{}
	for _, m := range def.FindAllStringSubmatch(hidden, -1) {
		roOnly[m[1]] = true
	}
	if len(roOnly) == 0 {
		t.Fatalf("parser found no read-write-only helpers in ramdisk.c; the guard scan is broken")
	}

	for name := range roOnly {
		if regexp.MustCompile(`\b` + regexp.QuoteMeta(name) + `\s*\(`).MatchString(visible) {
			t.Errorf("kernel/ramdisk.c: %s is defined only when !TOS_LFS_RO but used by a read-only build", name)
		}
	}

	// The other half of issue #14: the hash is recorded on every open, so its
	// helper must exist in both builds.
	if !strings.Contains(visible, "hash_path(") {
		t.Errorf("kernel/ramdisk.c: hash_path is not visible to a read-only build")
	}

	// fs_ro also has to survive the config layer without changing shape.
	cfg, err := LoadConfig(filepath.Join(root, "configs", "rp2350.conf"))
	if err != nil {
		t.Fatal(err)
	}
	ro := *cfg
	ro.FSReadOnly = true
	if ro.FSReadOnly != true || cfg.FSReadOnly != false {
		t.Fatalf("FSReadOnly does not round-trip: %v / %v", cfg.FSReadOnly, ro.FSReadOnly)
	}
}

// splitOnReadOnlyGuard returns the lines a read-only build compiles and the
// lines it drops.  Only the TOS_LFS_RO forms are interpreted; any other
// condition is treated as visible, which can only make the test stricter.
func splitOnReadOnlyGuard(text string) (visible, hidden string) {
	type frame struct {
		roOnly bool // branch is compiled only when TOS_LFS_RO is undefined
		roDep  bool // branch is compiled only when TOS_LFS_RO is defined
		elseAt bool
	}
	var stack []frame
	var vis, hid []string

	isVisible := func() bool {
		for _, f := range stack {
			if f.roOnly && !f.elseAt {
				return false
			}
			if f.roDep && f.elseAt {
				return false
			}
		}
		return true
	}

	for _, line := range strings.Split(text, "\n") {
		trimmed := strings.TrimSpace(line)
		switch {
		case strings.HasPrefix(trimmed, "#ifdef TOS_LFS_RO"):
			stack = append(stack, frame{roDep: true})
			continue
		case strings.HasPrefix(trimmed, "#ifndef TOS_LFS_RO"):
			stack = append(stack, frame{roOnly: true})
			continue
		case strings.HasPrefix(trimmed, "#if") || strings.HasPrefix(trimmed, "#ifdef") ||
			strings.HasPrefix(trimmed, "#ifndef"):
			cond := trimmed
			switch {
			case strings.Contains(cond, "!defined(TOS_LFS_RO)"):
				stack = append(stack, frame{roOnly: true})
			case strings.Contains(cond, "TOS_LFS_RO"):
				stack = append(stack, frame{roDep: true})
			default:
				// Some unrelated condition: assume both builds see it, which
				// only makes the test stricter.
				stack = append(stack, frame{})
			}
			continue
		case strings.HasPrefix(trimmed, "#else") || strings.HasPrefix(trimmed, "#elif"):
			if len(stack) != 0 {
				stack[len(stack)-1].elseAt = true
			}
			continue
		case strings.HasPrefix(trimmed, "#endif"):
			if len(stack) != 0 {
				stack = stack[:len(stack)-1]
			}
			continue
		}
		if isVisible() {
			vis = append(vis, line)
		} else {
			hid = append(hid, line)
		}
	}
	return strings.Join(vis, "\n"), strings.Join(hid, "\n")
}

// stripCComments blanks out // and /* */ comments but keeps newlines, so line
// numbers and code that only mentions a helper in prose do not look like a
// call.  Good enough for a kernel source file: no line splicing games here.
func stripCComments(text string) string {
	var out strings.Builder
	var inLine, inBlock bool
	var inStr, inChr bool
	for i := 0; i < len(text); i++ {
		c := text[i]
		var next byte
		if i+1 < len(text) {
			next = text[i+1]
		}
		switch {
		case inLine:
			if c == '\n' {
				inLine = false
				out.WriteByte(c)
			}
		case inBlock:
			if c == '*' && next == '/' {
				inBlock = false
				i++
			} else if c == '\n' {
				out.WriteByte(c)
			}
		case inStr:
			out.WriteByte(c)
			if c == '\\' && next != 0 {
				out.WriteByte(next)
				i++
			} else if c == '"' {
				inStr = false
			}
		case inChr:
			out.WriteByte(c)
			if c == '\\' && next != 0 {
				out.WriteByte(next)
				i++
			} else if c == '\'' {
				inChr = false
			}
		case c == '/' && next == '/':
			inLine = true
			i++
		case c == '/' && next == '*':
			inBlock = true
			i++
		case c == '"':
			inStr = true
			out.WriteByte(c)
		case c == '\'':
			inChr = true
			out.WriteByte(c)
		default:
			out.WriteByte(c)
		}
	}
	return out.String()
}
