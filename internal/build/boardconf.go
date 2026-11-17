package build

import (
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

// BoardConfig is the board-layer half of C1.  It deliberately contains only
// board facts: which SoC this is, which peripheral classes physically exist,
// the variant name, and the task-window geometry.  Chip facts (arch/triple/
// clocks/XIP/memory domains) stay in the chip/board Go layer; image policy
// (heap/procs/user trimming) stays in configs/<target>.conf.
type BoardConfig struct {
	Path        string
	Schema      int
	Chip        string
	Variant     string
	Peripherals map[string]bool
	UserExclude []string
	WindowGrain int
	WindowStack int
	WindowGuard int
}

// chipForBoard is the single board->chip mapping used while the tree still has
// flat board directories.  A board that shares a chip (rp2350 and rp2350rv)
// must still declare the same chip in board.conf.
func chipForBoard(board string) string {
	switch board {
	case "pc":
		return "pc"
	case "qemu-virt":
		return "qemu-virt"
	case "qemu-mps2":
		return "qemu-mps2"
	case "esp32c3":
		return "esp32c3"
	case "rp2040":
		return "rp2040"
	case "rp2350", "rp2350rv":
		return "rp2350"
	}
	return ""
}

// LoadBoardConfig reads board/<board>/board.conf.  The schema is checked here
// so a future key addition cannot silently change existing builds.
func LoadBoardConfig(root, board string) (*BoardConfig, error) {
	path := filepath.Join(root, "board", board, "board.conf")
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("%s: %w", relName(root, path), err)
	}
	sections, err := parseINI(string(data))
	if err != nil {
		return nil, fmt.Errorf("%s: %w", relName(root, path), err)
	}
	s := sections["board"]
	if s == nil {
		return nil, fmt.Errorf("%s: missing [board]", relName(root, path))
	}
	schema, err := strconv.Atoi(strings.TrimSpace(s["schema"]))
	if err != nil || schema != 1 {
		return nil, fmt.Errorf("%s: schema must be 1 (got %q)", relName(root, path), s["schema"])
	}
	chip := strings.TrimSpace(s["chip"])
	if chip == "" {
		return nil, fmt.Errorf("%s: chip is required", relName(root, path))
	}
	if want := chipForBoard(board); want == "" {
		return nil, fmt.Errorf("%s: no chip mapping for board %q", relName(root, path), board)
	} else if chip != want {
		return nil, fmt.Errorf("%s: chip = %q, but board %q is chip %q",
			relName(root, path), chip, board, want)
	}
	bc := &BoardConfig{
		Path:        path,
		Schema:      schema,
		Chip:        chip,
		Variant:     strings.TrimSpace(s["variant"]),
		Peripherals: map[string]bool{},
	}
	for _, p := range splitList(s["peripherals"]) {
		bc.Peripherals[strings.ToLower(p)] = true
	}
	/* [user] exclude is the hardware-absence list: programs this board can
	 * never run (no WiFi radio, no MPU for the protection demo).  A config's
	 * own [user] exclude is the separate, reversible "this image omits it"
	 * list; ApplyConfig adds them together. */
	if u := sections["user"]; u != nil {
		bc.UserExclude = splitList(u["exclude"])
	}
	w := sections["window"]
	if w == nil {
		return nil, fmt.Errorf("%s: missing [window]", relName(root, path))
	}
	if bc.WindowGrain, err = parseSize(w["grain"]); err != nil || bc.WindowGrain <= 0 {
		return nil, fmt.Errorf("%s: [window] grain: %v", relName(root, path), err)
	}
	if bc.WindowStack, err = parseSize(w["stack"]); err != nil || bc.WindowStack <= 0 {
		return nil, fmt.Errorf("%s: [window] stack: %v", relName(root, path), err)
	}
	if bc.WindowGuard, err = parseSize(w["guard"]); err != nil || bc.WindowGuard <= 0 {
		return nil, fmt.Errorf("%s: [window] guard: %v", relName(root, path), err)
	}
	return bc, nil
}

func featurePeripheral(name string) string {
	switch strings.ToLower(name) {
	case "wifi":
		return "wifi"
	case "pio":
		return "pio"
	case "lcd":
		return "lcd"
	}
	return ""
}

func (b *Builder) ensureBoardConfig() error {
	if b.BoardConfig != nil {
		return nil
	}
	if b.Target.Board == "" {
		return nil
	}
	bc, err := LoadBoardConfig(b.Root, b.Target.Board)
	if err != nil {
		return err
	}
	b.BoardConfig = bc
	return nil
}

// validateConfig rejects a config that contradicts the selected target's
// physical board.  This is deliberately before features/drivers are applied:
// a mistaken -target must fail instead of building an image with a driver the
// board does not have.
func (b *Builder) validateConfig(c *Config) error {
	if b.BoardConfig == nil {
		return nil
	}
	if c != nil && c.Explicit && c.Target != "" && b.Target.Name != "" && c.Target != b.Target.Name {
		return fmt.Errorf("config %s targets %s but build target is %s; use -target %s or remove -target",
			c.Path, c.Target, b.Target.Name, c.Target)
	}
	check := func(feature, source string) error {
		need := featurePeripheral(feature)
		if need == "" || b.BoardConfig.Peripherals[need] {
			return nil
		}
		return fmt.Errorf("%s enables %q but board %q has no %s peripheral",
			source, feature, b.Target.Board, need)
	}
	for _, f := range b.Target.DefaultFeatures {
		if err := check(f, "target "+b.Target.Name); err != nil {
			return err
		}
	}
	if c == nil {
		return nil
	}
	for f, on := range c.Features {
		if on {
			if err := check(f, "config "+c.Path); err != nil {
				return err
			}
		}
	}
	for _, d := range c.Drivers {
		p := filepath.ToSlash(d)
		if strings.Contains(p, "/wifi/") && !b.BoardConfig.Peripherals["wifi"] {
			return fmt.Errorf("config %s adds WiFi driver %q but board %q has no wifi peripheral",
				c.Path, d, b.Target.Board)
		}
	}
	return nil
}
