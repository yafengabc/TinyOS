package build

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestBoardConfigsExistForEveryBoard(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	for _, board := range []string{"pc", "qemu-virt", "qemu-mps2", "esp32c3", "rp2040",
		"rp2350-generic", "rp2350rv-generic", "atk-dnr2350am", "atk-dnr2350am-rv32", "pico2"} {
		bc, err := LoadBoardConfig(root, board)
		if err != nil {
			t.Errorf("%s: %v", board, err)
			continue
		}
		if bc.Schema != 1 || bc.Chip == "" || len(bc.Peripherals) == 0 {
			t.Errorf("%s: incomplete board config: %+v", board, bc)
		}
	}
}

func TestApplyConfigRejectsTargetConflict(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	b := New(root)
	b.Target = Targets["rp2040"]
	c := &Config{Path: "fake.conf", Explicit: true, Target: "esp32c3"}
	if err := b.ApplyConfig(c); err == nil || !strings.Contains(err.Error(), "build target is rp2040") {
		t.Fatalf("target conflict error = %v", err)
	}
}

func TestApplyConfigRejectsMissingPeripheral(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	b := New(root)
	b.Target = Targets["rp2040"]
	c := &Config{Path: "fake.conf", Target: "rp2040", Features: map[string]bool{"wifi": true}}
	if err := b.ApplyConfig(c); err == nil || !strings.Contains(err.Error(), "no wifi peripheral") {
		t.Fatalf("missing peripheral error = %v", err)
	}
}

func TestApplyConfigAcceptsBoardFeature(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	b := New(root)
	b.Target = Targets["rp2040"]
	c := &Config{Path: "fake.conf", Target: "rp2040", Features: map[string]bool{"pio": true}}
	if err := b.ApplyConfig(c); err != nil {
		t.Fatalf("pio on rp2040: %v", err)
	}
	if b.BoardConfig == nil || b.BoardConfig.Chip != "rp2040" {
		t.Fatalf("board config not loaded: %+v", b.BoardConfig)
	}
}

func TestBoardUserExcludes(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	wifi := []string{"wifiscan", "wificonn", "ping", "net", "dhcp", "host", "http"}
	// The generic baselines claim no panel, so the display programs are out
	// there; the ATK boards compile the panel driver and keep them.
	generic := append(append([]string{}, wifi...), "screen", "lcdconsole", "splash", "fb")
	want := map[string][]string{
		"rp2040":             append(append([]string{}, wifi...), "prot"),
		"rp2350-generic":     generic,
		"rp2350rv-generic":   generic,
		"atk-dnr2350am":      wifi,
		"atk-dnr2350am-rv32": wifi,
		"pico2":              generic,
	}
	for board, list := range want {
		bc, err := LoadBoardConfig(root, board)
		if err != nil {
			t.Errorf("%s: %v", board, err)
			continue
		}
		got := map[string]bool{}
		for _, n := range bc.UserExclude {
			got[n] = true
		}
		if len(got) != len(list) {
			t.Errorf("%s: %d excludes, want %d (%v)", board, len(got), len(list), bc.UserExclude)
			continue
		}
		for _, n := range list {
			if !got[n] {
				t.Errorf("%s: missing exclude %q", board, n)
			}
		}
	}
	/* Board-level exclusion is hardware truth: only the RP boards have one. */
	for _, board := range []string{"pc", "qemu-virt", "qemu-mps2", "esp32c3"} {
		bc, err := LoadBoardConfig(root, board)
		if err != nil {
			t.Errorf("%s: %v", board, err)
			continue
		}
		if len(bc.UserExclude) != 0 {
			t.Errorf("%s: unexpected excludes %v", board, bc.UserExclude)
		}
	}
}

func TestTargetsMatchChipFacts(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	for name, tgt := range Targets {
		b := New(root)
		b.Target = tgt
		if err := b.ensureBoardConfig(); err != nil {
			t.Errorf("%s: %v", name, err)
			continue
		}
		isa, ok := isaFor(b.BoardConfig.Chip, b.Target.Arch)
		if !ok {
			t.Errorf("%s: chip %s has no %s image", name, b.BoardConfig.Chip, b.Target.Arch)
			continue
		}
		if b.Target.Triple != isa.Triple || b.Target.Machine != isa.Machine {
			t.Errorf("%s: enriched triple/mcpu %q/%q, chip %s/%s says %q/%q",
				name, b.Target.Triple, b.Target.Machine,
				b.BoardConfig.Chip, b.Target.Arch, isa.Triple, isa.Machine)
		}
		if b.Target.FSBacking == "" || b.Target.RamdiskSectors <= 0 {
			t.Errorf("%s: board.conf did not fill fs_backing/ramdisk_sectors", name)
		}
	}
}

func TestBoardHeaderGeneration(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	b := New(root)
	b.Target = Targets["rp2040"]
	b.BuildDir = t.TempDir()
	if err := b.ensureBoardConfig(); err != nil {
		t.Fatal(err)
	}
	if err := b.writeBoardHeader(); err != nil {
		t.Fatal(err)
	}
	path := filepath.Join(b.BuildDir, "gen", "board_config.h")
	first, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{
		"#define TOS_BOARD_NAME \"rp2040\"",
		"#define TOS_CHIP_NAME \"rp2040\"",
		"#define TOS_APP_GRAIN 0x1000u",
		"#define TOS_HAS_WIFI 0",
		"#define TOS_HAS_PIO 1",
	} {
		if !strings.Contains(string(first), want) {
			t.Errorf("generated header missing %q", want)
		}
	}
	if err := b.writeBoardHeader(); err != nil {
		t.Fatal(err)
	}
	second, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(first) != string(second) {
		t.Fatal("generation is not deterministic")
	}
}

// A variant board (base = ...) must be wired for the same silicon as its
// base.  This replaces the old per-board allow list: with the list gone, the
// base is the only other statement of what the board is, so it is the check
// that a typo in chip = runs into.
func TestBoardVariantMatchesBaseChip(t *testing.T) {
	root := t.TempDir()
	write := func(board, conf string) {
		t.Helper()
		dir := filepath.Join(root, "board", board)
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "board.conf"), []byte(conf), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	conf := func(chip, extra string) string {
		return "[board]\nschema = 1\nchip = " + chip + "\n" + extra +
			"fs_backing = flash\nramdisk_sectors = 704\n" +
			"flash_bytes = 4m\nlfs_offset = 512k\n" +
			"[window]\ngrain = 4k\nstack = 4k\nguard = 4k\n"
	}

	write("base-arm", conf("rp2350", ""))
	write("variant-same", conf("rp2350", "base = base-arm\n"))
	if _, err := LoadBoardConfig(root, "variant-same"); err != nil {
		t.Fatalf("variant on the same silicon rejected: %v", err)
	}

	write("variant-other", conf("rp2040", "base = base-arm\n"))
	if _, err := LoadBoardConfig(root, "variant-other"); err == nil ||
		!strings.Contains(err.Error(), "silicon") {
		t.Fatalf("variant on a different chip was accepted: %v", err)
	}

	write("self", conf("rp2350", "base = self\n"))
	if _, err := LoadBoardConfig(root, "self"); err == nil ||
		!strings.Contains(err.Error(), "this board") {
		t.Fatalf("base = self was accepted: %v", err)
	}

	write("orphan", conf("rp2350", "base = nowhere\n"))
	if _, err := LoadBoardConfig(root, "orphan"); err == nil {
		t.Fatalf("base = nowhere was accepted")
	}
}

func TestBoardMissingWindowField(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "board", "rp2040")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	text := "[board]\nschema = 1\nchip = rp2040\nfs_backing = flash\n" +
		"ramdisk_sectors = 1200\nflash_bytes = 2m\nlfs_offset = 512k\n" +
		"[window]\nstack = 4k\nguard = 4k\n"
	if err := os.WriteFile(filepath.Join(dir, "board.conf"), []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
	_, err := LoadBoardConfig(root, "rp2040")
	if err == nil {
		t.Fatal("missing grain was accepted")
	}
	if !strings.Contains(err.Error(), "missing [window] grain") {
		t.Errorf("error does not name the missing key: %v", err)
	}
	if strings.Contains(err.Error(), "<nil>") {
		t.Errorf("error still prints <nil>: %v", err)
	}
}

// A flash-backed board has to say how big its flash is and where the
// filesystem starts.  The same silicon ships with 1, 2, 4 and 8 MiB parts, so
// "4 MiB" is not a safe default: the partition is derived from these two
// numbers, and a wrong one either wastes most of the chip or mounts a
// filesystem that runs past the end of it.
func TestBoardFlashGeometry(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "board", "rp2350")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	write := func(flash string) {
		t.Helper()
		text := "[board]\nschema = 1\nchip = rp2350\nfs_backing = flash\n" +
			"ramdisk_sectors = 704\n" + flash +
			"[window]\ngrain = 4k\nstack = 4k\nguard = 4k\n"
		if err := os.WriteFile(filepath.Join(dir, "board.conf"), []byte(text), 0o644); err != nil {
			t.Fatal(err)
		}
	}

	write("flash_bytes = 4m\nlfs_offset = 512k\n")
	bc, err := LoadBoardConfig(root, "rp2350")
	if err != nil {
		t.Fatalf("4m/512k rejected: %v", err)
	}
	if bc.LFSBlocks() != 896 {
		t.Errorf("4 MiB with a 512 KiB offset is 896 blocks, got %d", bc.LFSBlocks())
	}

	write("flash_bytes = 2m\nlfs_offset = 512k\n")
	bc, err = LoadBoardConfig(root, "rp2350")
	if err != nil {
		t.Fatalf("2m/512k rejected: %v", err)
	}
	if bc.LFSBlocks() != 384 {
		t.Errorf("2 MiB with a 512 KiB offset is 384 blocks, got %d", bc.LFSBlocks())
	}

	write("flash_bytes = 4m\n")
	if _, err := LoadBoardConfig(root, "rp2350"); err == nil ||
		!strings.Contains(err.Error(), "lfs_offset") {
		t.Errorf("missing lfs_offset was accepted or misnamed: %v", err)
	}

	write("flash_bytes = 512k\nlfs_offset = 512k\n")
	if _, err := LoadBoardConfig(root, "rp2350"); err == nil ||
		!strings.Contains(err.Error(), "no room") {
		t.Errorf("partition at the end of the chip was accepted or misnamed: %v", err)
	}

	write("flash_bytes = 256k\nlfs_offset = 64k\n")
	if _, err := LoadBoardConfig(root, "rp2350"); err == nil ||
		!strings.Contains(err.Error(), "does not fit") {
		t.Errorf("a ramdisk larger than the flash was accepted or misnamed: %v", err)
	}
}

func TestBoardConfigRejectsBadSchema(t *testing.T) {
	dir := t.TempDir()
	if err := os.MkdirAll(filepath.Join(dir, "board", "rp2040"), 0o755); err != nil {
		t.Fatal(err)
	}
	text := "[board]\nschema = 2\nchip = rp2040\n[window]\ngrain = 4k\nstack = 4k\nguard = 4k\n"
	if err := os.WriteFile(filepath.Join(dir, "board", "rp2040", "board.conf"), []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadBoardConfig(dir, "rp2040"); err == nil {
		t.Fatal("schema 2 was accepted")
	}
}
