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
