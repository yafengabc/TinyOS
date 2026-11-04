package main

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

// cmdEmu implements `tostest emu build|test`, the Go port of scripts/emu.py:
// build the rv32emu-c submodule into build/rv32emu-c.exe, then boot the
// esp32c3 kernel on it and poke the shell.
func cmdEmu(args []string) int {
	action := "test"
	if len(args) > 0 && (args[0] == "build" || args[0] == "test") {
		action = args[0]
		args = args[1:]
	}
	_ = flag.NewFlagSet("emu", flag.ExitOnError).Parse(args)

	switch action {
	case "build":
		if emuBuild() {
			return 0
		}
		return 1
	default:
		if !fileExists(findEmu()) {
			fmt.Fprintf(os.Stderr, "missing %s: run 'tostest emu build'\n", findEmu())
			return 1
		}
		return emuSmoke()
	}
}

func emuSubmoduleReady() bool {
	return fileExists(filepath.Join(repoRoot(), "rv32emu-c", "src", "main.c"))
}

// emuBuild compiles the rv32emu-c submodule with its own Go build tool and
// copies the result into build/rv32emu-c.exe.  It reuses the compiler paths
// tos already knows about (TOS_BIN, then the MSYS2 UCRT64 default).
func emuBuild() bool {
	if !emuSubmoduleReady() {
		fmt.Fprintln(os.Stderr, "rv32emu-c/ submodule is missing; run:\n  git submodule update --init")
		return false
	}
	cmd := exec.Command("go", "run", "./cmd/rv32emu-build", "emu")
	cmd.Dir = filepath.Join(repoRoot(), "rv32emu-c")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	extra := splitList(os.Getenv("TOS_BIN"))
	if def := `C:\msys64\ucrt64\bin`; isDir(def) {
		extra = append(extra, def)
	}
	if len(extra) > 0 {
		cmd.Env = append(os.Environ(),
			"PATH="+strings.Join(extra, string(os.PathListSeparator))+string(os.PathListSeparator)+os.Getenv("PATH"))
	}
	if err := cmd.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "submodule build failed: %v\n", err)
		return false
	}
	src := filepath.Join(repoRoot(), "rv32emu-c", "build", exeName("rv32emu-c"))
	if !fileExists(src) {
		fmt.Fprintf(os.Stderr, "build finished but %s is missing\n", src)
		return false
	}
	data, err := os.ReadFile(src)
	if err != nil {
		fmt.Fprintf(os.Stderr, "reading build output: %v\n", err)
		return false
	}
	out := findEmu()
	if err := os.MkdirAll(filepath.Dir(out), 0o755); err != nil {
		fmt.Fprintf(os.Stderr, "mkdir: %v\n", err)
		return false
	}
	if err := os.WriteFile(out, data, 0o755); err != nil {
		fmt.Fprintf(os.Stderr, "writing %s: %v\n", out, err)
		return false
	}
	fmt.Printf("ok -> %s (%d bytes)\n", relPath(out), len(data))
	return true
}

// emuSmoke boots the esp32c3 kernel on RV32EMU-C and checks uptime + ls.
func emuSmoke() int {
	elf := filepath.Join(repoRoot(), "build", "kernel-esp32c3.elf")
	rd := filepath.Join(repoRoot(), "build", "ramdisk-esp32c3.bin")
	for _, p := range []string{elf, rd} {
		if !fileExists(p) {
			fmt.Fprintf(os.Stderr, "missing %s: run 'tos build -target esp32c3'\n", p)
			return 1
		}
	}

	s, err := startEmulator(findEmu(), elf, rd)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	defer s.close()

	t0 := time.Now()
	ok, _ := s.waitFor([]byte("tos> "), 10*time.Second)
	boot := time.Since(t0)
	if !ok {
		fmt.Fprintln(os.Stderr, "no shell prompt; last output:")
		fmt.Fprintln(os.Stderr, s.tail(800))
		return 1
	}
	fmt.Printf("boot: %.3f s\n", boot.Seconds())

	if err := s.sendLine("uptime", "\r\n"); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	// The marker used to be "ms (" from a previous uptime format; the current
	// line is "uptime: 0.820 s (820 ms)  tick=..." so match the label.
	ok, _ = s.waitFor([]byte("uptime:"), 5*time.Second)
	if !ok {
		fmt.Fprintln(os.Stderr, "`uptime` produced no answer; last output:")
		fmt.Fprintln(os.Stderr, s.tail(800))
		return 1
	}
	// the marker may land mid-line; let the rest of the line arrive
	s.read(100 * time.Millisecond)
	for _, line := range strings.Split(s.text(), "\n") {
		if strings.Contains(line, "uptime:") {
			fmt.Println("uptime: " + strings.TrimSpace(line))
			break
		}
	}

	if err := s.sendLine("ls /", "\r\n"); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	ok, _ = s.waitFor([]byte("bin/sh"), 5*time.Second)
	if !ok {
		fmt.Fprintln(os.Stderr, "`ls` did not list the filesystem")
		return 1
	}
	fmt.Println("filesystem: ok")
	fmt.Println("SMOKE TEST OK")
	return 0
}

// startEmulator boots a kernel under RV32EMU-C with the console wired to our
// pipe.
//
// -clk wall is not a detail.  Since 4e8cf59 the emulator drives the guest
// timer from the instruction count by default, which is the right model for
// a firmware benchmark and the wrong one for TinyOS: the emulated chip
// reports 20 MHz (the PLL registers are zero, see the [clk] line), so the
// instruction-driven TIMG fires about every 1000 instructions, while the
// 160 MHz part it stands in for fires every 160000.  Our slice is one tick,
// so at that density the scheduler switches context between almost every
// pair of instructions of the guest and a program never gets through a
// syscall sequence - the C3 kernel hangs before printing its first line.
// With -clk wall the TIMG counts host microseconds, the tick rate is what
// the guest programmed (1000 Hz), and a millisecond is a millisecond again.
func startEmulator(emu, elf, ramdisk string) (*session, error) {
	cmd := exec.Command(emu, "-elf", elf, "-ramdisk", ramdisk,
		"-rxdelay", "5000000", "-rxrate", "64", "-rxperiod", "20000",
		"-clk", "wall")
	return startSession(cmd, true)
}

func splitList(s string) []string {
	var out []string
	for _, p := range strings.Split(s, string(os.PathListSeparator)) {
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}

func isDir(p string) bool {
	fi, err := os.Stat(p)
	return err == nil && fi.IsDir()
}

func relPath(p string) string {
	r, err := filepath.Rel(repoRoot(), p)
	if err != nil {
		return p
	}
	return r
}

func fileExists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}
