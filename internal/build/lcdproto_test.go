package build

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// B-4a/B-4b: the ST7789 stack is one protocol layer plus one driver, shared by
// the Cortex-M33 and Hazard3 images of the same panel.
//
//	drivers/display/st7789/lcd_proto.h    the panel sequence (what the
//	                                      controller is told)
//	drivers/display/st7789/lcd_st7789.c   the bus and the pixel layer, with
//	                                      three #if defined(__riscv) spots
//	drivers/display/st7789/font5x7.h      the cell font
//
// The two ports used to be separate directories with byte-identical sources.
// Nothing here can run on a host for real, so this pins the shape: a copy of
// the panel sequence, a per-ISA driver directory, or a second font is a
// regression waiting to happen.
func TestLcdIsSharedBetweenRPVariants(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	dir := filepath.Join(root, "drivers", "display", "st7789")

	// The protocol layer, still where it was.
	proto, err := os.ReadFile(filepath.Join(dir, "lcd_proto.h"))
	if err != nil {
		t.Fatalf("shared protocol layer missing: %v", err)
	}
	for _, want := range []string{"struct lcd_proto_ops", "lcd_proto_window",
		"lcd_proto_panel_init"} {
		if !strings.Contains(string(proto), want) {
			t.Errorf("lcd_proto.h: no %s", want)
		}
	}

	// One driver, next to the protocol layer.
	impl, err := os.ReadFile(filepath.Join(dir, "lcd_st7789.c"))
	if err != nil {
		t.Fatalf("driver missing: %v", err)
	}
	driver := string(impl)
	for _, want := range []string{"lcd_proto_panel_init(&proto_ops)",
		"lcd_proto_window(&proto_ops"} {
		if !strings.Contains(driver, want) {
			t.Errorf("lcd_st7789.c: does not call %s", want)
		}
	}
	// The panel sequence belongs to the protocol layer.
	for _, gone := range []string{"lcd_cmd_data",
		"static const uint8_t madctl[]", "static const uint8_t pvgam[]",
		"lcd_cmd(0x2a)", "lcd_cmd(0x2b)", "lcd_cmd(0x2c)"} {
		if strings.Contains(driver, gone) {
			t.Errorf("lcd_st7789.c: still carries the panel sequence (%q)", gone)
		}
	}
	// The three per-ISA spots are the reason this file can be shared; if they
	// disappear the merge stopped being a merge.
	for _, want := range []string{"defined(__riscv)", "arm_memmap.h", "rp2350rv_memmap.h"} {
		if !strings.Contains(driver, want) {
			t.Errorf("lcd_st7789.c: no %q - the ISA switch is gone", want)
		}
	}

	// No per-ISA driver directories, and neither board silently keeps one.
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range entries {
		if e.IsDir() {
			t.Errorf("drivers/display/st7789/%s: a per-ISA copy is back", e.Name())
		}
	}
	for _, board := range []string{"atk-dnr2350am", "atk-dnr2350am-rv32"} {
		body, err := os.ReadFile(filepath.Join(root, "board", board, "board.conf"))
		if err != nil {
			t.Fatal(err)
		}
		text := string(body)
		if !strings.Contains(text, "drivers = ") ||
			!strings.Contains(text, "drivers/display/st7789,") &&
				!strings.Contains(text, "drivers/display/st7789\n") {
			t.Errorf("%s/board.conf: does not list the shared ST7789 driver", board)
		}
		if strings.Contains(text, "st7789/rp2350") {
			t.Errorf("%s/board.conf: still points at the per-ISA ST7789 copy", board)
		}
	}

	// One font, and one copy of the init table, in the whole driver tree.
	var fonts, tables int
	err = filepath.Walk(dir, func(p string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}
		switch filepath.Base(p) {
		case "font5x7.h":
			fonts++
		}
		if !strings.HasSuffix(p, ".c") && !strings.HasSuffix(p, ".h") {
			return nil
		}
		b, err := os.ReadFile(p)
		if err != nil {
			return err
		}
		tables += strings.Count(string(b), "static const uint8_t pvgam[]")
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	if fonts != 1 {
		t.Errorf("font5x7.h appears %d times, want 1", fonts)
	}
	if tables != 1 {
		t.Errorf("vendor init table appears %d times, want 1 (lcd_proto.h)", tables)
	}
}
