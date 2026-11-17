package build

import (
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"
)

// The board tree has three layers: a chip baseline (rp2350-generic), an ISA
// overlay for the other core (rp2350rv-generic), and concrete boards that add
// wiring (atk-dnr2350am, atk-dnr2350am-rv32, rpi-pico2).  A concrete board that
// starts carrying silicon code, or a second copy of the ATK device table,
// means the split has collapsed back into per-board forks.
func TestBoardLayering(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}

	chain := func(board string) []string {
		t.Helper()
		bc, err := LoadBoardConfig(root, board)
		if err != nil {
			t.Fatalf("%s: %v", board, err)
		}
		return bc.Bases
	}

	want := map[string][]string{
		"rp2350-generic":     nil,
		"rp2350rv-generic":   {"rp2350-generic"},
		"atk-dnr2350am":      {"rp2350-generic"},
		"atk-dnr2350am-rv32": {"rp2350rv-generic", "atk-dnr2350am", "rp2350-generic"},
		"rpi-pico2":          {"rp2350-generic"},
	}
	for board, wantChain := range want {
		got := chain(board)
		if strings.Join(got, ",") != strings.Join(wantChain, ",") {
			t.Errorf("%s: base chain %v, want %v", board, got, wantChain)
		}
	}

	// The old names were the board and the chip at once; they must be gone.
	for _, gone := range []string{"rp2350", "rp2350rv"} {
		if _, err := os.Stat(filepath.Join(root, "board", gone)); err == nil {
			t.Errorf("board/%s still exists: the chip baseline and the board are split now", gone)
		}
	}

	// A concrete board supplies data, not silicon: the only C it may carry is
	// its device table.  The RV32 ATK board needs no C at all - it takes the
	// RISC-V port from one base and the ATK table from the other.
	cFiles := func(board string) []string {
		t.Helper()
		entries, err := os.ReadDir(filepath.Join(root, "board", board))
		if err != nil {
			t.Fatal(err)
		}
		var out []string
		for _, e := range entries {
			if !e.IsDir() && strings.HasSuffix(e.Name(), ".c") {
				out = append(out, e.Name())
			}
		}
		sort.Strings(out)
		return out
	}
	if got := cFiles("atk-dnr2350am"); strings.Join(got, ",") != "board_data.c" {
		t.Errorf("atk-dnr2350am carries %v, want only board_data.c", got)
	}
	if got := cFiles("atk-dnr2350am-rv32"); len(got) != 0 {
		t.Errorf("atk-dnr2350am-rv32 carries %v; it should get everything from its bases", got)
	}
	if got := cFiles("rpi-pico2"); strings.Join(got, ",") != "board_data.c" {
		t.Errorf("rpi-pico2 carries %v, want only board_data.c", got)
	}

	// The chip baselines own the port: every non-data C file of the tree's
	// RP2350 support must live in one of them.
	for _, need := range []struct{ dir, file string }{
		{"rp2350-generic", "platform.c"},
		{"rp2350-generic", "layout.c"},
		{"rp2350-generic", "board_devs.c"},
		{"rp2350rv-generic", "platform.c"},
		{"rp2350rv-generic", "kernel.ld"},
	} {
		if _, err := os.Stat(filepath.Join(root, "board", need.dir, need.file)); err != nil {
			t.Errorf("board/%s/%s is missing", need.dir, need.file)
		}
	}
}

// Flags and include paths must follow the chip or the ISA, never the board
// name.  When board/rp2350 became board/atk-dnr2350am, three ARMv8-M flags
// (TOS_ARM_MPU_V8, TOS_SYSCALL_TRACE, TOS_ARM_SIO_SPIN) and TOS_COPY_DATA
// stopped being defined, and the image booted with ARMv7-M MPU registers on an
// M33: every task switch faulted and the shell rebooted in a loop.  A build
// passes either way, which is exactly why this is a source guard.
func TestNoBoardNameSelectsBuildFlags(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	dir := filepath.Join(root, "internal", "build")
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range entries {
		if e.IsDir() || filepath.Ext(e.Name()) != ".go" || strings.HasSuffix(e.Name(), "_test.go") {
			continue
		}
		body, err := os.ReadFile(filepath.Join(dir, e.Name()))
		if err != nil {
			t.Fatal(err)
		}
		for i, line := range strings.Split(string(body), "\n") {
			trimmed := strings.TrimSpace(line)
			if strings.HasPrefix(trimmed, "//") {
				continue
			}
			if !strings.Contains(trimmed, "Target.Board ==") &&
				!strings.Contains(trimmed, "Target.Board !=") {
				continue
			}
			// The empty check is about "is a board set at all", not about
			// which board it is.
			if strings.Contains(trimmed, `== ""`) || strings.Contains(trimmed, `!= ""`) {
				continue
			}
			t.Errorf("%s:%d: build behaviour selected by board name: %s",
				e.Name(), i+1, trimmed)
		}
	}
}
