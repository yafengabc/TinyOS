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
// Every source file that mentions TOS_LFS_RO is walked, so the next helper that
// lands on the wrong side of the guard is caught wherever it is written.
func TestReadOnlyBuildDoesNotUseROOnlyHelpers(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}

	// One definition pattern: a file-scope function.  Locals cannot be
	// "read-write only", so matching definitions inside functions is enough.
	def := regexp.MustCompile(`(?m)^static\s+[A-Za-z_][A-Za-z0-9_ \t*]*\b([a-z_][a-z0-9_]*)\s*\(`)

	checked := 0
	// Only the trees that are compiled into an image: walking the whole repo
	// (docs, goc32, tools) would make this test slower than the thing it
	// protects.
	for _, dir := range []string{"kernel", "board", "arch", "drivers", "include", "user"} {
		err = filepath.Walk(filepath.Join(root, dir), func(p string, info os.FileInfo, err error) error {
			if err != nil || info.IsDir() {
				return err
			}
			if !strings.HasSuffix(p, ".c") && !strings.HasSuffix(p, ".h") {
				return nil
			}
			body, err := os.ReadFile(p)
			if err != nil {
				return err
			}
			text := string(body)
			if !strings.Contains(text, "TOS_LFS_RO") {
				return nil
			}
			checked++
			rel, _ := filepath.Rel(root, p)
			visible, hidden := splitOnReadOnlyGuard(stripCComments(text))

			roOnly := map[string]bool{}
			for _, m := range def.FindAllStringSubmatch(hidden, -1) {
				roOnly[m[1]] = true
			}
			for name := range roOnly {
				if regexp.MustCompile(`\b` + regexp.QuoteMeta(name) + `\s*\(`).MatchString(visible) {
					t.Errorf("%s: %s is defined only when !TOS_LFS_RO but used by a read-only build",
						filepath.ToSlash(rel), name)
				}
			}
			return nil
		})
		if err != nil {
			t.Fatal(err)
		}
	}
	if checked == 0 {
		t.Fatalf("no file mentions TOS_LFS_RO; the fs_ro build is unreachable and this test proves nothing")
	}

	// The other half of issue #14: the hash is recorded on every open, so its
	// helper must exist in both builds.
	body, err := os.ReadFile(filepath.Join(root, "kernel", "ramdisk.c"))
	if err != nil {
		t.Fatal(err)
	}
	visible, _ := splitOnReadOnlyGuard(stripCComments(string(body)))
	if !strings.Contains(visible, "hash_path(") {
		t.Errorf("kernel/ramdisk.c: hash_path is not visible to a read-only build")
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
