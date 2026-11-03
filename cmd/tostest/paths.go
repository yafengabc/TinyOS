package main

import (
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
)

// repoRoot is the TinyOS checkout that contains go.mod.  tostest is normally
// run from the repo root (`go run ./cmd/tostest ...`), but walking up makes it
// robust against being invoked from a subdirectory.
func repoRoot() string {
	dir, err := os.Getwd()
	if err != nil {
		dir = "."
	}
	for {
		if _, err := os.Stat(filepath.Join(dir, "go.mod")); err == nil {
			return dir
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "."
		}
		dir = parent
	}
}

func exeName(base string) string {
	if runtime.GOOS == "windows" {
		return base + ".exe"
	}
	return base
}

// findEmu resolves the standard test runtime: $TOS_EMU, then the binary built
// from the rv32emu-c submodule (build/rv32emu-c.exe, see cmdEmu build).
func findEmu() string {
	if p := os.Getenv("TOS_EMU"); p != "" {
		return p
	}
	return filepath.Join(repoRoot(), "build", exeName("rv32emu-c"))
}

// findQEMU locates the QEMU binary for the given target name, mirroring the
// old smoke.ps1 search: $TOS_QEMU, common install locations, TOS_BIN, PATH.
func findQEMU(name string) string {
	if p := os.Getenv("TOS_QEMU"); p != "" {
		return p
	}
	var cands []string
	if pf := os.Getenv("ProgramFiles"); pf != "" {
		cands = append(cands, filepath.Join(pf, "qemu", name))
	}
	if la := os.Getenv("LOCALAPPDATA"); la != "" {
		cands = append(cands,
			filepath.Join(la, "qemu", name),
			filepath.Join(la, "Programs", "qemu", name))
	}
	cands = append(cands,
		filepath.Join(`C:\msys64\ucrt64\bin`, name),
		filepath.Join(`C:\msys64\mingw64\bin`, name))
	for _, d := range filepath.SplitList(os.Getenv("TOS_BIN")) {
		if d != "" {
			cands = append(cands, filepath.Join(d, name))
		}
	}
	for _, c := range cands {
		if _, err := os.Stat(c); err == nil {
			return c
		}
	}
	if p, err := exec.LookPath(name); err == nil {
		return p
	}
	return ""
}

func qemuName(target string) string {
	switch target {
	case "rv32", "rv32smp":
		return exeName("qemu-system-riscv32")
	case "arm":
		return exeName("qemu-system-arm")
	default:
		return exeName("qemu-system-i386")
	}
}
