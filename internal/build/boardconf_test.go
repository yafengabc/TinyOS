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
	for _, board := range []string{"pc", "qemu-virt", "qemu-mps2", "esp32c3", "rp2040", "rp2350", "rp2350rv"} {
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
	want := map[string][]string{
		"rp2040":   append(append([]string{}, wifi...), "prot"),
		"rp2350":   wifi,
		"rp2350rv": wifi,
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

func TestBoardChipPolicy(t *testing.T) {
	root := t.TempDir()
	write := func(board, conf string) {
		dir := filepath.Join(root, "board", board)
		if err := os.MkdirAll(dir, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "board.conf"), []byte(conf), 0o644); err != nil {
			t.Fatal(err)
		}
	}
	conf := func(chip string) string {
		return "[board]\nschema = 1\nchip = " + chip +
			"\nfs_backing = flash\nramdisk_sectors = 704\n" +
			"[window]\ngrain = 4k\nstack = 4k\nguard = 4k\n"
	}

	write("rp2040", conf("rp2350"))
	if _, err := LoadBoardConfig(root, "rp2040"); err == nil ||
		!strings.Contains(err.Error(), "not valid for board") {
		t.Fatalf("rp2040 with chip=rp2350 was accepted: %v", err)
	}

	/* The same silicon under the other ISA must stay legal. */
	write("rp2350rv", conf("rp2350"))
	if _, err := LoadBoardConfig(root, "rp2350rv"); err != nil {
		t.Fatalf("rp2350rv with chip=rp2350 rejected: %v", err)
	}
}

func TestBoardMissingWindowField(t *testing.T) {
	root := t.TempDir()
	dir := filepath.Join(root, "board", "rp2040")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	text := "[board]\nschema = 1\nchip = rp2040\nfs_backing = flash\n" +
		"ramdisk_sectors = 1200\n[window]\nstack = 4k\nguard = 4k\n"
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
