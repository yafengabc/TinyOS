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
		Name:  name,
		Board: strings.TrimSpace(s["board"]),
		Arch:  strings.TrimSpace(s["arch"]),
		March: strings.TrimSpace(s["march"]),
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
