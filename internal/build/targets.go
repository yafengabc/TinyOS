package build

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// Targets is the image list, loaded from targets/<name>.conf at package init.
//
// C1's three layers put image policy here and board facts in board.conf:
// a target only names a board, an ISA and the image knobs (heap, process
// slots, kernel stack).  Keeping the list in files instead of a Go literal is
// what makes "add a board" a data change: the same board directory can be
// built with different heaps, and a new board needs no code at all.
//
// The map type stays because every caller (build, doctor, the menu, tests)
// already treats it as a lookup table; only where it comes from changed.
var Targets = map[string]Target{}

// targetsErr is what LoadTargets returned at init.  Empty when the package
// was imported outside the source tree, where there is nothing to load.
var targetsErr error

func init() {
	targetsErr = LoadTargets()
}

// TargetsError reports why the image list is empty.  Doctor prints it; tests
// assert it is nil.
func TargetsError() error { return targetsErr }

// LoadTargets reads targets/*.conf and replaces the table in one step: a
// half-loaded list would silently drop images from the menu and from CI.
func LoadTargets() error {
	root, err := FindRoot()
	if err != nil {
		return err
	}
	dir := filepath.Join(root, "targets")
	entries, err := os.ReadDir(dir)
	if err != nil {
		return fmt.Errorf("targets/: %w", err)
	}
	loaded := map[string]Target{}
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".conf") {
			continue
		}
		t, err := loadTargetFile(filepath.Join(dir, e.Name()))
		if err != nil {
			return err
		}
		if _, dup := loaded[t.Name]; dup {
			return fmt.Errorf("%s: target %q is defined twice", relName(root, filepath.Join(dir, e.Name())), t.Name)
		}
		loaded[t.Name] = t
	}
	// Aliases are input-only: the map holds them so `-target <old name>`
	// still works, but each alias resolves to the canonical Target, so the
	// artifacts keep the board name.  A menu that lists Targets must skip
	// entries whose key differs from Name.
	for _, t := range loaded {
		for _, a := range t.Aliases {
			if a == t.Name {
				continue
			}
			if other, exists := loaded[a]; exists && other.Name != t.Name {
				return fmt.Errorf("targets/: alias %q of %q collides with target %q", a, t.Name, other.Name)
			}
			loaded[a] = t
		}
	}
	if len(loaded) == 0 {
		return fmt.Errorf("targets/: no *.conf files")
	}
	Targets = loaded
	return nil
}

func loadTargetFile(path string) (Target, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return Target{}, err
	}
	if err := validateINI(path, string(data), targetSchema); err != nil {
		return Target{}, err
	}
	sections, err := parseINI(string(data))
	if err != nil {
		return Target{}, fmt.Errorf("%s: %w", path, err)
	}
	s := sections["target"]
	if s == nil {
		return Target{}, fmt.Errorf("%s: missing [target]", path)
	}
	name := strings.TrimSpace(s["name"])
	if name == "" {
		name = strings.TrimSuffix(filepath.Base(path), ".conf")
	}
	t := Target{
		Name:    name,
		Aliases: splitList(s["aliases"]),
		Board:   strings.TrimSpace(s["board"]),
		Arch:    strings.TrimSpace(s["arch"]),
		March:   strings.TrimSpace(s["march"]),
		RCCArch: strings.TrimSpace(s["rcc_arch"]),
		Opt:     strings.TrimSpace(s["opt"]),
		UserOpt: strings.TrimSpace(s["user_opt"]),
	}
	if err := validOpt(t.Opt); err != nil {
		return Target{}, fmt.Errorf("%s: opt %w", path, err)
	}
	if err := validOpt(t.UserOpt); err != nil {
		return Target{}, fmt.Errorf("%s: user_opt %w", path, err)
	}
	if t.Board == "" {
		return Target{}, fmt.Errorf("%s: board is required", path)
	}
	intField := func(key string) (int, error) {
		raw := strings.TrimSpace(s[key])
		if raw == "" {
			return 0, nil
		}
		n, err := strconv.Atoi(raw)
		if err != nil || n < 0 {
			return 0, fmt.Errorf("%s: %s must be a non-negative number (got %q)", path, key, raw)
		}
		return n, nil
	}
	sizeField := func(key string) (int, error) {
		raw := strings.TrimSpace(s[key])
		if raw == "" {
			return 0, nil
		}
		n, err := parseSize(raw)
		if err != nil || n < 0 {
			return 0, fmt.Errorf("%s: %s is not a size (got %q)", path, key, raw)
		}
		return n, nil
	}
	switch t.RCCArch {
	case "":
	case "rv32":
		if t.Arch != "rv32" {
			return Target{}, fmt.Errorf("%s: rcc_arch = rv32 needs arch = rv32", path)
		}
	case "thumb1", "thumb2":
		if t.Arch != "arm" {
			return Target{}, fmt.Errorf("%s: rcc_arch = %s needs arch = arm", path, t.RCCArch)
		}
	default:
		return Target{}, fmt.Errorf("%s: rcc_arch must be rv32, thumb1 or thumb2 (got %q)",
			path, t.RCCArch)
	}
	if t.CPUs, err = intField("cpus"); err != nil {
		return Target{}, err
	}
	if t.MaxProcs, err = intField("max_procs"); err != nil {
		return Target{}, err
	}
	if t.HeapBytes, err = sizeField("heap"); err != nil {
		return Target{}, err
	}
	if t.KStack, err = sizeField("kstack"); err != nil {
		return Target{}, err
	}
	return t, nil
}
