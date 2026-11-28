package build

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

// boardPeripherals is the fixed vocabulary written into board_config.h.  A
// board.conf may list others; they simply do not get a TOS_HAS_* define.
var boardPeripherals = []string{
	"gpio", "uart", "usb", "spi", "i2c", "pwm", "dma", "pio", "lcd", "wifi", "flash",
}

// writeBoardHeader renders build/gen/board_config.h from board.conf.  It is a
// derived artifact: never committed, regenerated on every build, and only
// consumed by the kernel's boot-time cross-check (kernel/board_check.c).  The
// source of truth stays board.conf; the C side compares the generated numbers
// with layout.c's constants instead of trusting either copy.
func (b *Builder) writeBoardHeader() error {
	bc := b.BoardConfig
	if bc == nil {
		return nil
	}
	dir := filepath.Join(b.BuildDir, "gen")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	var sb strings.Builder
	fmt.Fprintf(&sb, "/* Generated from board/%s/board.conf - do not edit. */\n", b.Target.Board)
	sb.WriteString("#ifndef TINYOS_BOARD_CONFIG_H\n#define TINYOS_BOARD_CONFIG_H\n\n")
	fmt.Fprintf(&sb, "#define TOS_BOARD_NAME %q\n", b.Target.Board)
	fmt.Fprintf(&sb, "#define TOS_CHIP_NAME %q\n", bc.Chip)
	fmt.Fprintf(&sb, "#define TOS_VARIANT_NAME %q\n", bc.Variant)
	sb.WriteString("\n")
	fmt.Fprintf(&sb, "#define TOS_APP_GRAIN %#xu\n", uint32(bc.WindowGrain))
	fmt.Fprintf(&sb, "#define TOS_APP_STACK %#xu\n", uint32(bc.WindowStack))
	fmt.Fprintf(&sb, "#define TOS_APP_GUARD %#xu\n", uint32(bc.WindowGuard))
	fmt.Fprintf(&sb, "#define TOS_APP_STACK_ALIGN %d\n", bc.WindowStackAlign)
	fmt.Fprintf(&sb, "#define TOS_APP_STACK_AREA %#xu\n", uint32(bc.WindowStackArea))
	sb.WriteString("\n")
	/* Flash geometry, for boards that mount littlefs straight off the QSPI
	 * chip.  Same contract as the window numbers above: layout.c declares
	 * them too and board_config_check() refuses to boot if the two disagree. */
	if bc.FSBacking == "flash" {
		fmt.Fprintf(&sb, "#define TOS_FLASH_BYTES %#xu\n", uint32(bc.FlashBytes))
		fmt.Fprintf(&sb, "#define TOS_LFS_OFFSET %#xu\n", uint32(bc.LFSOffset))
		fmt.Fprintf(&sb, "#define TOS_LFS_BLOCKS %du\n", uint32(bc.LFSBlocks()))
		sb.WriteString("\n")
	}
	if b.Target.XipFlashBase != 0 {
		fmt.Fprintf(&sb, "#define TOS_XIP_MAP_BASE %#xu\n", b.Target.XipMapBase)
		fmt.Fprintf(&sb, "#define TOS_XIP_CODE_OFFSET %#xu\n", b.Target.XipFlashBase)
		sb.WriteString("\n")
	}
	for _, p := range boardPeripherals {
		v := 0
		if bc.Peripherals[p] {
			v = 1
		}
		fmt.Fprintf(&sb, "#define TOS_HAS_%s %d\n", strings.ToUpper(p), v)
	}
	sb.WriteString("\n#endif\n")

	path := filepath.Join(dir, "board_config.h")
	if err := os.WriteFile(path, []byte(sb.String()), 0o644); err != nil {
		return err
	}
	b.logf("      %s\n", relName(b.Root, path))
	return nil
}
