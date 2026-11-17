package build

import (
	"os"
	"path/filepath"
	"testing"
)

// The image list moved out of Go into targets/*.conf.  These tests are the
// few things a literal used to guarantee for free: every image exists, its
// board and ISA resolve, and the knobs are the values the images were sized
// against (a silently dropped `heap = 60k` would still build).
func TestTargetsLoadFromConf(t *testing.T) {
	if err := TargetsError(); err != nil {
		t.Fatalf("targets did not load: %v", err)
	}
	if err := LoadTargets(); err != nil {
		t.Fatalf("reload failed: %v", err)
	}
	want := map[string]struct {
		board string
		arch  string
		march string
		cpus  int
		heap  int
	}{
		"x86":                    {"pc", "x86", "", 0, 256 * 1024},
		"rv32":                   {"qemu-virt", "rv32", "rv32imc", 0, 256 * 1024},
		"esp32c3":                {"esp32c3", "rv32", "rv32imc", 0, 80 * 1024},
		"arm":                    {"qemu-mps2", "arm", "", 0, 128 * 1024},
		"rpi-pico":               {"rpi-pico", "arm", "", 0, 8 * 1024},
		"rpi-pico-smp":           {"rpi-pico", "arm", "", 2, 8 * 1024},
		"atk-dnr2350am":          {"atk-dnr2350am", "arm", "", 0, 60 * 1024},
		"atk-dnr2350am-smp":      {"atk-dnr2350am", "arm", "", 2, 60 * 1024},
		"atk-dnr2350am-rv32":     {"atk-dnr2350am-rv32", "rv32", "rv32imac", 0, 32 * 1024},
		"atk-dnr2350am-rv32-smp": {"atk-dnr2350am-rv32", "rv32", "rv32imac", 2, 32 * 1024},
		"rpi-pico2":              {"rpi-pico2", "arm", "", 0, 60 * 1024},
		"rv32smp":                {"qemu-virt", "rv32", "rv32imac", 2, 256 * 1024},
		"rv32nc":                 {"qemu-virt", "rv32", "rv32im", 0, 256 * 1024},
	}

	// The map also carries the old names as aliases; only entries whose key
	// is their own Name are images.
	canonical := 0
	for key, tgt := range Targets {
		if key == tgt.Name {
			canonical++
		}
	}
	if canonical != len(want) {
		t.Errorf("%d canonical targets, want %d", canonical, len(want))
	}
	for name, w := range want {
		tgt, ok := Targets[name]
		if !ok {
			t.Errorf("%s: not loaded", name)
			continue
		}
		if tgt.Board != w.board || tgt.Arch != w.arch || tgt.March != w.march ||
			tgt.CPUs != w.cpus || tgt.HeapBytes != w.heap {
			t.Errorf("%s: got board=%s arch=%s march=%s cpus=%d heap=%d, want %s/%s/%s/%d/%d",
				name, tgt.Board, tgt.Arch, tgt.March, tgt.CPUs, tgt.HeapBytes,
				w.board, w.arch, w.march, w.cpus, w.heap)
		}
	}

}

// A target must name a board that exists and an ISA that board's chip has.
// This used to be boardChips' job; the check belongs to the data now.
func TestTargetsResolveTheirBoardAndISA(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	for name, tgt := range Targets {
		bc, err := LoadBoardConfig(root, tgt.Board)
		if err != nil {
			t.Errorf("%s: %v", name, err)
			continue
		}
		chip, ok := Chips[bc.Chip]
		if !ok {
			t.Errorf("%s: board %s names unknown chip %s", name, tgt.Board, bc.Chip)
			continue
		}
		if _, ok := chip.ISAs[tgt.Arch]; !ok {
			t.Errorf("%s: board %s (%s) has no %s ISA", name, tgt.Board, bc.Chip, tgt.Arch)
		}
	}
}

// The conf files are data; a stray file in targets/ must not become an image.
func TestTargetsDirOnlyHoldsTargets(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	dir := filepath.Join(root, "targets")
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range entries {
		if e.IsDir() {
			t.Errorf("targets/%s: directories are not target definitions", e.Name())
			continue
		}
		if filepath.Ext(e.Name()) != ".conf" && e.Name() != "README.md" {
			t.Errorf("targets/%s: unexpected file", e.Name())
		}
	}
}
