package build

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

// The on-disk layout of a bootable image is written down in three places that
// cannot see each other: the boot loader's #define, the Go constants used to
// pad the image, and the board's PLAT_LFS_LBA that tells the kernel where the
// filesystem starts.  Nothing links them, and getting one wrong is quiet:
//
//   - KernelSectors larger than the boot loader's copy loop means the image is
//     silently truncated on disk - the kernel runs (it is at the start) until
//     it touches the missing tail;
//   - KernelSectors smaller than the loader's means the loader reads past the
//     kernel into the filesystem partition, and the extra sectors land on top
//     of whatever the loader has already put at 0x10000..;
//   - a stale PLAT_LFS_LBA makes littlefs mount the kernel's own tail (or
//     nothing at all) and report whatever superblock it finds there.
//
// So the constants are checked against each other on every build instead of
// being kept in sync by hand.

var (
	reDefineKernelSectors = regexp.MustCompile(`(?m)^\s*#define\s+KERNEL_SECTORS\s+([0-9A-Fa-fx]+)\b`)
	rePlatLFSLBA          = regexp.MustCompile(`(?m)^\s*(?:static\s+)?const\s+uint32_t\s+PLAT_LFS_LBA\s*=\s*([^;]+);`)
)

// checkLayout cross-checks the sector counts against the sources that are not
// compiled by Go.  It runs before anything is compiled so a mismatch costs a
// second, not a confusing runtime failure.
func (b *Builder) checkLayout() error {
	stage1 := filepath.Join(b.Root, "boot", "stage1.S")
	n, err := constFromSource(b.Root, stage1, reDefineKernelSectors, "KERNEL_SECTORS")
	if err != nil {
		return err
	}
	if n != KernelSectors {
		return fmt.Errorf("%s loads %d sectors (%d KiB) but internal/build sets KernelSectors = %d: "+
			"change both, and move PLAT_LFS_LBA on the disk-backed boards with them",
			relName(b.Root, stage1), n, n*512/1024, KernelSectors)
	}

	// Only the ATA image packs boot + kernel + filesystem back to back, so it
	// is the only layout where the filesystem LBA follows from the sector
	// counts.  Flash-backed boards place the filesystem at a fixed offset of
	// their own (1 MiB on the C3 and on the RP2040) and embedded ones at zero.
	if b.Target.FSBacking != "disk" {
		return nil
	}
	layout := filepath.Join(b.Root, "board", b.Target.Board, "layout.c")
	lba, err := constFromSource(b.Root, layout, rePlatLFSLBA, "PLAT_LFS_LBA")
	if err != nil {
		return err
	}
	if lba != LFSPartitionLBA {
		return fmt.Errorf("board/%s/layout.c puts the filesystem at LBA %d, but the image packs it at %d "+
			"(BootSectors %d + KernelSectors %d)",
			b.Target.Board, lba, LFSPartitionLBA, BootSectors, KernelSectors)
	}
	return nil
}

// constFromSource returns the value of one integer constant in a C source
// file.  The expressions used for these are plain arithmetic on decimal or
// hex literals with an optional u suffix, which is all this needs to handle.
func constFromSource(root, path string, re *regexp.Regexp, name string) (int, error) {
	src, err := os.ReadFile(path)
	if err != nil {
		return 0, fmt.Errorf("%s: %w", relName(root, path), err)
	}
	m := re.FindStringSubmatch(string(src))
	if m == nil {
		return 0, fmt.Errorf("%s: no definition of %s", relName(root, path), name)
	}
	v, err := evalIntExpr(strings.TrimSpace(m[1]))
	if err != nil {
		return 0, fmt.Errorf("%s: %s = %q: %w", relName(root, path), name, m[1], err)
	}
	return v, nil
}

// evalIntExpr parses "181", "0x100000u / 512u", "2 * 3 + 4" and the like.
func evalIntExpr(s string) (int, error) {
	toks, err := lexIntExpr(s)
	if err != nil {
		return 0, err
	}
	v, i, err := parseSum(toks, 0)
	if err != nil {
		return 0, err
	}
	if i != len(toks) {
		return 0, fmt.Errorf("trailing %q", toks[i])
	}
	return v, nil
}

func lexIntExpr(s string) ([]string, error) {
	var toks []string
	for i := 0; i < len(s); {
		c := s[i]
		switch {
		case c == ' ' || c == '\t' || c == '(' || c == ')':
			i++
		case c == '+' || c == '-' || c == '*' || c == '/':
			toks = append(toks, string(c))
			i++
		case c >= '0' && c <= '9':
			j := i
			for j < len(s) && (isNumChar(s[j])) {
				j++
			}
			toks = append(toks, s[i:j])
			i = j
		default:
			return nil, fmt.Errorf("unexpected %q", string(c))
		}
	}
	return toks, nil
}

func isNumChar(c byte) bool {
	return c >= '0' && c <= '9' || c >= 'a' && c <= 'f' || c >= 'A' && c <= 'F' ||
		c == 'x' || c == 'X' || c == 'u' || c == 'U'
}

// parseSum handles left-associative + and -, calling parseProduct for terms.
func parseSum(toks []string, i int) (int, int, error) {
	v, i, err := parseProduct(toks, i)
	if err != nil {
		return 0, i, err
	}
	for i < len(toks) && (toks[i] == "+" || toks[i] == "-") {
		op := toks[i]
		var rhs int
		rhs, i, err = parseProduct(toks, i+1)
		if err != nil {
			return 0, i, err
		}
		if op == "+" {
			v += rhs
		} else {
			v -= rhs
		}
	}
	return v, i, nil
}

func parseProduct(toks []string, i int) (int, int, error) {
	v, i, err := parseNumber(toks, i)
	if err != nil {
		return 0, i, err
	}
	for i < len(toks) && (toks[i] == "*" || toks[i] == "/") {
		op := toks[i]
		var rhs int
		rhs, i, err = parseNumber(toks, i+1)
		if err != nil {
			return 0, i, err
		}
		if op == "*" {
			v *= rhs
		} else {
			if rhs == 0 {
				return 0, i, fmt.Errorf("division by zero")
			}
			v /= rhs
		}
	}
	return v, i, nil
}

func parseNumber(toks []string, i int) (int, int, error) {
	if i >= len(toks) {
		return 0, i, fmt.Errorf("expression ends early")
	}
	lit := strings.TrimRight(toks[i], "uU")
	v, err := strconv.ParseInt(lit, 0, 64)
	if err != nil {
		return 0, i, fmt.Errorf("bad number %q", toks[i])
	}
	return int(v), i + 1, nil
}
