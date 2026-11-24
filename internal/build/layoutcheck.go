package build

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// The window geometry is written twice: board.conf is the board fact the build
// sizes images with, layout.c is the port's constant the kernel compares at
// boot (kernel/board_check.c panics on a mismatch).  Catching it at boot is too
// late for an image that was already flashed, so the build reads the same three
// numbers out of the layout.c it will link and refuses to disagree.  It has
// happened: board/atk-dnr2350am-rv32 kept the ARM half's 4 KiB grain after
// board/rp2350rv-generic moved to 16 KiB, and nothing noticed because no
// emulator models that board.

var layoutConstRe = map[string]*regexp.Regexp{
	"grain": regexp.MustCompile(`PLAT_APP_GRAIN\s*=\s*([^;]+);`),
	"stack": regexp.MustCompile(`PLAT_STACK_BYTES\s*=\s*([^;]+);`),
	"guard": regexp.MustCompile(`PLAT_HEAP_GUARD\s*=\s*([^;]+);`),
}

// cNumber evaluates the integer forms layout.c uses: "0x4000u", "4096",
// "4u * 1024u".  Anything else is an error rather than a silent zero, so a
// future edit to layout.c has to extend this instead of disabling the check.
func cNumber(expr string) (int, error) {
	v := 1
	for _, term := range strings.Split(expr, "*") {
		t := strings.TrimSpace(term)
		t = strings.TrimSuffix(strings.TrimSuffix(t, "u"), "U")
		t = strings.TrimSuffix(strings.TrimSuffix(t, "l"), "L")
		var n uint64
		var err error
		if strings.HasPrefix(t, "0x") || strings.HasPrefix(t, "0X") {
			n, err = strconv.ParseUint(t[2:], 16, 32)
		} else {
			n, err = strconv.ParseUint(t, 10, 32)
		}
		if err != nil {
			return 0, fmt.Errorf("cannot read %q as a number", strings.TrimSpace(term))
		}
		v *= int(n)
	}
	return v, nil
}

// layoutNumbers reads grain/stack/guard from the layout.c that this board
// resolves to (the variant's own file first, then each base in order).
func (b *Builder) layoutNumbers() (map[string]int, string, error) {
	path := b.boardFile("layout.c")
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, path, nil // a board with no layout.c of its own: nothing to check
	}
	text := string(data)
	out := map[string]int{}
	for name, re := range layoutConstRe {
		m := re.FindStringSubmatch(text)
		if m == nil {
			return nil, path, fmt.Errorf("%s: no %s= initialiser", relName(b.Root, path), name)
		}
		v, err := cNumber(m[1])
		if err != nil {
			return nil, path, fmt.Errorf("%s: %s: %w", relName(b.Root, path), name, err)
		}
		out[name] = v
	}
	return out, path, nil
}

// checkLayoutMatchesConf compares the three window numbers before anything is
// built.  The kernel would panic at boot instead, which a board without an
// emulator never reaches in CI.
func (b *Builder) checkLayoutMatchesConf() error {
	if b.BoardConfig == nil {
		return nil
	}
	got, path, err := b.layoutNumbers()
	if err != nil || got == nil {
		return err
	}
	conf := map[string]int{
		"grain": b.BoardConfig.WindowGrain,
		"stack": b.BoardConfig.WindowStack,
		"guard": b.BoardConfig.WindowGuard,
	}
	for _, name := range []string{"grain", "stack", "guard"} {
		if got[name] != conf[name] {
			return fmt.Errorf("%s: [window] %s = %d but %s has %d; the kernel panics at boot on this "+
				"(kernel/board_check.c), so fix the board.conf value or the layout.c constant",
				filepath.Join("board", b.Target.Board, "board.conf"), name, conf[name],
				relName(b.Root, path), got[name])
		}
	}
	return nil
}
