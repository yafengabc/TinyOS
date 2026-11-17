package build

import (
	"errors"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// The mock half of A-7: compile the shared ST7789 protocol against a lower
// that records instead of transmitting, then read the recording.
//
// Nothing here can boot a panel, so this is what "the driver still says the
// same thing to the controller" means on a build machine: the exact command
// order, the vendor table's bytes and the address-window arithmetic are
// pinned in Go, and the numbers below are the panel's, not a snapshot of
// whatever the C file last emitted.
func TestLcdProtocolMockLower(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	cc, err := hostCC()
	if err != nil {
		t.Skip(err)
	}

	dir := t.TempDir()
	copyFile(t, filepath.Join(root, "drivers", "display", "st7789", "lcd_proto.h"),
		filepath.Join(dir, "lcd_proto.h"))
	copyFile(t, filepath.Join(root, "internal", "build", "testdata", "lcdproto_mock.c"),
		filepath.Join(dir, "harness.c"))
	// lcd_proto.h includes "types.h" for uint8_t/uint32_t; the kernel's copy
	// pulls in half the world, and the harness only needs the scalar types.
	if err := os.WriteFile(filepath.Join(dir, "types.h"), []byte("#include <stdint.h>\n"), 0o644); err != nil {
		t.Fatal(err)
	}

	exe := filepath.Join(dir, "harness")
	if runtime.GOOS == "windows" {
		exe += ".exe"
	}
	build := exec.Command(cc[0], append(cc[1:], "-std=c99", "-Wall", "-Wextra", "-I", dir,
		filepath.Join(dir, "harness.c"), "-o", exe)...)
	build.Dir = dir
	if out, err := build.CombinedOutput(); err != nil {
		t.Fatalf("compiling the mock lower failed: %v\n%s", err, out)
	}

	run := func(mode string) []string {
		cmd := exec.Command(exe, mode)
		cmd.Dir = dir
		out, err := cmd.CombinedOutput()
		if err != nil {
			t.Fatalf("running the mock lower (mode %s) failed: %v\n%s", mode, err, out)
		}
		return strings.Fields(string(out))
	}

	// Panel init: reset, sleep-out, the vendor table, inversion on, display on.
	// The pvgam/nvgam bytes are the ATK panel's; if a panel is ever swapped
	// this list is what has to change, on purpose.
	want := []string{
		"W20",
		"C01", "W150", // SWRESET
		"C11", "W120", // SLPOUT
		"C36", "D70", // MADCTL: landscape, RGB
		"C3a", "D05", // COLMOD: 16 bpp
		"Cb2", "D0c0c003333", // PORCTRL
		"Cb7", "D35", // GCTRL
		"Cbb", "D19", // VCOMS
		"Cc0", "D2c", // LCMCTRL
		"Cc2", "D01", // VDVVRHEN
		"Cc3", "D12", // VRHS
		"Cc4", "D20", // VDVS
		"Cc6", "D01", // FRCTRL2
		"Cd0", "Da4a1", // PWCTRL1
		"Ce0", "Dd0040d11132b3f544c180d0b1f23", // PVGAMCTRL
		"Ce1", "Dd0040c11132c3f44512f1f1f2023", // NVGAMCTRL
		"C21", // INVER on: the panel is IPS
		"C29", // DISPON
		"W20",
	}
	if got := run("0"); !equalTokens(got, want) {
		t.Errorf("panel init sequence changed\n got: %s\nwant: %s",
			strings.Join(got, " "), strings.Join(want, " "))
	}

	// Address window, in panel coordinates with the 40/53 origin offset, plus
	// the two degenerate cases the caller relies on being dropped.
	win := []string{
		"C2a", "D00280117", // CASET 40..279
		"C2b", "D003500bb", // RASET 53..187
		"C2c", // RAMWR
		"C2a", "D00280117",
		"C2b", "D003500bb",
		"C2c",
		"C2a", "D002e0033", // 6x8 cell at (6,8): 46..51
		"C2b", "D003d0044", // 61..68
		"C2c",
	}
	if got := run("1"); !equalTokens(got, win) {
		t.Errorf("address window changed\n got: %s\nwant: %s",
			strings.Join(got, " "), strings.Join(win, " "))
	}
}

func equalTokens(got, want []string) bool {
	if len(got) != len(want) {
		return false
	}
	for i := range got {
		if got[i] != want[i] {
			return false
		}
	}
	return true
}

func copyFile(t *testing.T, from, to string) {
	t.Helper()
	b, err := os.ReadFile(from)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(to, b, 0o644); err != nil {
		t.Fatal(err)
	}
}

// hostCC finds a compiler that can build a hosted C program: TOS_CC first, so
// a developer's own clang wins, then whatever is on PATH.
func hostCC() ([]string, error) {
	if v := strings.TrimSpace(os.Getenv("TOS_CC")); v != "" {
		fields := strings.Fields(v)
		if _, err := exec.LookPath(fields[0]); err == nil {
			return fields, nil
		}
	}
	for _, name := range []string{"clang", "cc", "gcc"} {
		if p, err := exec.LookPath(name); err == nil {
			return []string{p}, nil
		}
	}
	return nil, errors.New("no host C compiler (set TOS_CC or install clang)")
}
