package build

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// B-4a: the ST7789 protocol layer (reset/sleep-out order, the vendor init
// table, MADCTL/COLMOD, the address window, RAMWR) lives once, in
// drivers/display/st7789/lcd_proto.h.  The two RP2350 ports keep only the bus:
// CS/DC, SPI bytes, DMA and a millisecond delay.
//
// Nothing here can run on the host, so this pins the seam the way the RP park
// test does: a port that grows its own copy of the panel sequence is a
// regression, because next time only one copy gets fixed.
func TestLcdProtocolIsShared(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}

	proto := filepath.Join(root, "drivers", "display", "st7789", "lcd_proto.h")
	body, err := os.ReadFile(proto)
	if err != nil {
		t.Fatalf("shared protocol layer missing: %v", err)
	}
	shared := string(body)
	for _, want := range []string{
		"struct lcd_proto_ops",
		"lcd_proto_window",
		"lcd_proto_panel_init",
	} {
		if !strings.Contains(shared, want) {
			t.Errorf("lcd_proto.h: no %s", want)
		}
	}

	ports := []string{
		filepath.Join(root, "drivers", "display", "st7789", "rp2350", "lcd_st7789.c"),
		filepath.Join(root, "drivers", "display", "st7789", "rp2350rv", "lcd_st7789.c"),
	}
	for _, port := range ports {
		body, err := os.ReadFile(port)
		if err != nil {
			t.Fatal(err)
		}
		text := string(body)
		name := filepath.Base(filepath.Dir(port))
		if !strings.Contains(text, `#include "../lcd_proto.h"`) {
			t.Errorf("%s: does not include ../lcd_proto.h", name)
		}
		for _, want := range []string{"lcd_proto_panel_init(&proto_ops)", "lcd_proto_window(&proto_ops"} {
			if !strings.Contains(text, want) {
				t.Errorf("%s: does not call %s", name, want)
			}
		}
		// The lower owns the bus, the upper owns the panel commands.  These
		// strings should only ever appear in the shared header now.
		for _, gone := range []string{
			"lcd_cmd_data", // per-command + payload helper, superseded
			"static const uint8_t madctl[]",
			"static const uint8_t pvgam[]",
			"lcd_cmd(0x2a)", // CASET
			"lcd_cmd(0x2b)", // RASET
			"lcd_cmd(0x2c)", // RAMWR
		} {
			if strings.Contains(text, gone) {
				t.Errorf("%s: still carries the panel sequence (%q)", name, gone)
			}
		}
	}

	// One copy of the init table across the whole driver tree.
	var tables int
	err = filepath.Walk(filepath.Join(root, "drivers", "display", "st7789"), func(p string, info os.FileInfo, err error) error {
		if err != nil || info.IsDir() || (!strings.HasSuffix(p, ".c") && !strings.HasSuffix(p, ".h")) {
			return err
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
	if tables != 1 {
		t.Errorf("vendor init table appears %d times, want 1 (drivers/display/st7789/lcd_proto.h)", tables)
	}
}
