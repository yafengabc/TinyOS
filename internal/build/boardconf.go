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
	Path    string
	Schema  int
	Chip    string
	Variant string
	// Base names another board directory whose sources this one reuses:
	// same silicon, different wiring.  Files in this board's directory
	// shadow same-named files in the base (board_devs.c, board_data.c, ...),
	// so a variant writes only what differs and never copies platform.c.
	Base string
	// Bases is Base resolved through any further base links, nearest first.
	// board/pico2 -> rp2350-generic is one hop; board/atk-rv32 ->
	// rp2350rv-generic -> rp2350-generic is two, and the compiler needs the
	// whole chain to know which files are inherited and which are replaced.
	Bases       []string
	Peripherals map[string]bool
	UserExclude []string
	// Board-layer build facts.  ensureBoardConfig() copies these into the
	// Target, so Targets only names the board and the image knobs.
	FSBacking      string
	RamdiskSectors int
	XipFlashBase   uint32
	// Flash geometry, for boards whose filesystem lives on the QSPI chip.
	// Both are board facts: the same silicon with a different flash part is a
	// different board (or a different variant of one), not a different image.
	FlashBytes      int
	LFSOffset       int
	Drivers         []string
	DefaultFeatures []string
	WindowGrain     int
	WindowStack     int
	WindowGuard     int
}

// LFSBlocks is the size of the littlefs partition in 4 KiB blocks: from the
// offset to the end of the flash.  The flashed image is much smaller; the
// filesystem grows into the rest on first boot.
func (bc *BoardConfig) LFSBlocks() int {
	if bc.FlashBytes <= bc.LFSOffset {
		return 0
	}
	return (bc.FlashBytes - bc.LFSOffset) / 4096
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
	if _, ok := Chips[chip]; !ok {
		return nil, fmt.Errorf("%s: unknown chip %q", relName(root, path), chip)
	}
	/* A board says which chip it is wired for; there is no per-board allow
	 * list any more.  Two things catch a wrong `chip =`: a variant must name
	 * the same silicon as its base, and the target's ISA must exist on that
	 * chip (TestTargetsResolveTheirBoardAndISA / isaFor).  A chip that is
	 * simply wrong for the board usually fails to compile within seconds,
	 * because the board's platform code includes the port's headers. */
	bc := &BoardConfig{
		Path:        path,
		Schema:      schema,
		Chip:        chip,
		Variant:     strings.TrimSpace(s["variant"]),
		Base:        strings.TrimSpace(s["base"]),
		Peripherals: map[string]bool{},
	}
	/* base may name more than one board: the ISA overlay and the board-data
	 * overlay are orthogonal, so board/atk-dnr2350am-rv32 is
	 * "rp2350rv-generic, atk-dnr2350am" - the RISC-V port's code plus the
	 * ATK wiring - instead of a third copy of both.  Each name expands
	 * through its own chain; earlier names win, like include paths. */
	if bc.Base != "" {
		/* base may name more than one board: the ISA overlay and the board
		 * data are orthogonal, so board/atk-dnr2350am-rv32 is
		 * "rp2350rv-generic, atk-dnr2350am" - the RISC-V port's code plus the
		 * ATK wiring - instead of a third copy of both.
		 *
		 * The walk is breadth-first so depth means priority: every direct
		 * base comes before its own base, which keeps a board's file ahead of
		 * the chip baseline's file of the same name.  `seen` also makes a
		 * diamond (two bases sharing a parent) resolve once and a loop
		 * terminate. */
		seen := map[string]bool{board: true}
		queue := splitList(bc.Base)
		for len(queue) > 0 {
			name := queue[0]
			queue = queue[1:]
			if name == board {
				return nil, fmt.Errorf("%s: base = %q is this board", relName(root, path), name)
			}
			if seen[name] {
				continue
			}
			seen[name] = true
			base, err := LoadBoardConfig(root, name)
			if err != nil {
				return nil, fmt.Errorf("%s: base = %q: %w", relName(root, path), name, err)
			}
			if base.Chip != bc.Chip {
				return nil, fmt.Errorf("%s: chip = %q but base %q is %q silicon",
					relName(root, path), bc.Chip, name, base.Chip)
			}
			bc.Bases = append(bc.Bases, name)
			queue = append(queue, splitList(base.Base)...)
			if len(bc.Bases) > 8 {
				return nil, fmt.Errorf("%s: base chain is deeper than 8 boards", relName(root, path))
			}
		}
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
	bc.FSBacking = strings.TrimSpace(s["fs_backing"])
	switch bc.FSBacking {
	case "disk", "embedded", "flash":
	default:
		return nil, fmt.Errorf("%s: fs_backing must be disk, embedded or flash (got %q)",
			relName(root, path), bc.FSBacking)
	}
	if bc.RamdiskSectors, err = strconv.Atoi(strings.TrimSpace(s["ramdisk_sectors"])); err != nil || bc.RamdiskSectors <= 0 {
		return nil, fmt.Errorf("%s: ramdisk_sectors must be a positive number", relName(root, path))
	}
	/* A board that mounts its filesystem straight off the QSPI chip has to
	 * say how big that chip is and where the partition starts: the same
	 * silicon ships with 1, 2, 4 or 8 MiB parts, and getting this wrong is
	 * the difference between a filesystem that stops at the end of the chip
	 * and one that walks off it. */
	if bc.FSBacking == "flash" {
		size := func(key string) (int, error) {
			raw := strings.TrimSpace(s[key])
			if raw == "" {
				return 0, fmt.Errorf("%s: missing [board] %s (required when fs_backing = flash)", relName(root, path), key)
			}
			v, perr := parseSize(raw)
			if perr != nil || v <= 0 {
				return 0, fmt.Errorf("%s: invalid [board] %s %q", relName(root, path), key, raw)
			}
			return v, nil
		}
		if bc.FlashBytes, err = size("flash_bytes"); err != nil {
			return nil, err
		}
		if bc.LFSOffset, err = size("lfs_offset"); err != nil {
			return nil, err
		}
		if bc.LFSOffset%4096 != 0 {
			return nil, fmt.Errorf("%s: lfs_offset %d is not a multiple of 4096",
				relName(root, path), bc.LFSOffset)
		}
		if bc.LFSOffset >= bc.FlashBytes {
			return nil, fmt.Errorf("%s: lfs_offset %d leaves no room in %d bytes of flash",
				relName(root, path), bc.LFSOffset, bc.FlashBytes)
		}
		if img := bc.RamdiskSectors * 512; img > bc.FlashBytes-bc.LFSOffset {
			return nil, fmt.Errorf("%s: ramdisk (%d bytes) does not fit after lfs_offset in %d bytes of flash",
				relName(root, path), img, bc.FlashBytes)
		}
	}
	if v := strings.TrimSpace(s["xip_flash_base"]); v != "" {
		n, perr := strconv.ParseUint(v, 0, 32)
		if perr != nil {
			return nil, fmt.Errorf("%s: xip_flash_base: %v", relName(root, path), perr)
		}
		bc.XipFlashBase = uint32(n)
	}
	bc.Drivers = splitList(s["drivers"])
	bc.DefaultFeatures = splitList(s["features"])
	w := sections["window"]
	if w == nil {
		return nil, fmt.Errorf("%s: missing [window]", relName(root, path))
	}
	/* Distinguish "key absent" from "value invalid": parseSize returns
	 * (0, nil) for a missing key, which used to print as "<nil>". */
	windowSize := func(key string) (int, error) {
		raw := strings.TrimSpace(w[key])
		if raw == "" {
			return 0, fmt.Errorf("%s: missing [window] %s", relName(root, path), key)
		}
		v, perr := parseSize(raw)
		if perr != nil || v <= 0 {
			return 0, fmt.Errorf("%s: invalid [window] %s %q", relName(root, path), key, raw)
		}
		return v, nil
	}
	if bc.WindowGrain, err = windowSize("grain"); err != nil {
		return nil, err
	}
	if bc.WindowStack, err = windowSize("stack"); err != nil {
		return nil, err
	}
	if bc.WindowGuard, err = windowSize("guard"); err != nil {
		return nil, err
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

// boardFile resolves a board-layer file, letting a variant board fall back to
// the directory named by board.conf's `base =`.  A file present in the
// variant directory wins; this is the same rule the compiler applies to
// sources, just for the files Go reads itself (layout.c, kernel.ld).
func (b *Builder) boardFile(name string) string {
	variant := filepath.Join(b.Root, "board", b.Target.Board, name)
	if _, err := os.Stat(variant); err == nil {
		return variant
	}
	if b.BoardConfig != nil {
		for _, base := range b.BoardConfig.Bases {
			p := filepath.Join(b.Root, "board", base, name)
			if _, err := os.Stat(p); err == nil {
				return p
			}
		}
	}
	return variant
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
	if err := b.Target.applyBoard(bc); err != nil {
		return err
	}
	b.BoardConfig = bc
	return nil
}

// applyBoard fills the derived fields from board.conf and the chip table.
// Targets literals only name the board and the image knobs (arch/march/cpus/
// heap/kstack); everything else has one source.
func (t *Target) applyBoard(bc *BoardConfig) error {
	isa, ok := isaFor(bc.Chip, t.Arch)
	if !ok {
		return fmt.Errorf("board %s: chip %s has no %q image", t.Board, bc.Chip, t.Arch)
	}
	t.Triple = isa.Triple
	t.Machine = isa.Machine
	t.FSBacking = bc.FSBacking
	t.RamdiskSectors = bc.RamdiskSectors
	t.XipFlashBase = bc.XipFlashBase
	t.Drivers = bc.Drivers
	t.DefaultFeatures = bc.DefaultFeatures
	t.winGrain = bc.WindowGrain
	t.winStack = bc.WindowStack
	t.winGuard = bc.WindowGuard
	return nil
}

// TargetByName returns a target with the board-derived fields filled in.
// Callers without a Builder (menuconfig, small tools) use this instead of
// reading Targets directly; when no source tree is found the literal is
// returned unchanged so an installed binary still starts.
func TargetByName(name string) (Target, bool) {
	t, ok := Targets[name]
	if !ok {
		return t, false
	}
	root, err := FindRoot()
	if err != nil {
		return t, true
	}
	bc, err := LoadBoardConfig(root, t.Board)
	if err != nil {
		return t, true
	}
	if err := t.applyBoard(bc); err != nil {
		return t, true
	}
	return t, true
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
