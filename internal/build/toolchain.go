package build

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
)

// Toolchain describes how to compile and link freestanding code.
type Toolchain struct {
	CC []string
	LD []string
	// UsesZig reports that CC/LD go through "zig cc", whose argument
	// handling differs from a plain clang driver (it rejects -Wl,-pie).
	UsesZig bool
}

func (t Toolchain) String() string {
	return strings.Join(t.CC, " ") + "  |  " + strings.Join(t.LD, " ")
}

// SearchPaths are the usual places a Windows user has MSYS2 / LLVM / QEMU.
func SearchPaths() []string {
	var dirs []string
	if v := os.Getenv("TOS_BIN"); v != "" {
		dirs = append(dirs, strings.Split(v, string(os.PathListSeparator))...)
	}
	dirs = append(dirs,
		`C:\msys64\ucrt64\bin`,
		`C:\msys64\mingw64\bin`,
		`C:\msys64\usr\bin`,
		`C:\Program Files\LLVM\bin`,
		`C:\Program Files\qemu`,
	)
	if la := os.Getenv("LOCALAPPDATA"); la != "" {
		// the QEMU for Windows installer defaults to %LOCALAPPDATA%\qemu,
		// but user-scope installs commonly land in Programs\qemu as well.
		dirs = append(dirs, filepath.Join(la, "qemu"), filepath.Join(la, "Programs", "qemu"))
	}
	return dirs
}

func findTool(names ...string) string {
	for _, n := range names {
		if p, err := exec.LookPath(n); err == nil {
			return p
		}
	}
	exts := []string{""}
	if runtime.GOOS == "windows" {
		exts = []string{".exe", ""}
	}
	for _, dir := range SearchPaths() {
		for _, n := range names {
			for _, ext := range exts {
				p := filepath.Join(dir, n+ext)
				if st, err := os.Stat(p); err == nil && !st.IsDir() {
					return p
				}
			}
		}
	}
	return ""
}

// toolchain.go no longer looks up QEMU: qemu.go owns the target-aware
// search (qemu-system-i386 for x86, qemu-system-riscv32 for rv32).

// findZig locates a plain Zig installation.  Zig ships a clang driver
// ("zig cc") that can cross-compile to i386-freestanding and link ELF with
// lld, so it is a handy all-in-one toolchain on Windows.
func findZig() string {
	if p, err := exec.LookPath("zig"); err == nil {
		return p
	}

	var dirs []string
	if v := os.Getenv("TOS_BIN"); v != "" {
		dirs = append(dirs, strings.Split(v, string(os.PathListSeparator))...)
	}
	dirs = append(dirs, SearchPaths()...)
	for _, dir := range dirs {
		for _, name := range []string{"zig.exe", "zig"} {
			p := filepath.Join(dir, name)
			if st, err := os.Stat(p); err == nil && !st.IsDir() {
				return p
			}
		}
	}

	// Common unpacked / package-manager locations.
	patterns := []string{
		filepath.Join(os.Getenv("LOCALAPPDATA"), "Programs", "zig", "*", "zig.exe"),
		filepath.Join(os.Getenv("LOCALAPPDATA"), "zig", "*", "zig.exe"),
		filepath.Join(os.Getenv("USERPROFILE"), "scoop", "apps", "zig", "current", "zig.exe"),
		filepath.Join(os.Getenv("ProgramFiles"), "zig", "zig.exe"),
		`C:\zig\zig.exe`,
		`C:\Program Files\zig\zig.exe`,
	}
	var hits []string
	for _, pat := range patterns {
		if m, err := filepath.Glob(pat); err == nil {
			hits = append(hits, m...)
		}
	}
	if len(hits) > 0 {
		sort.Strings(hits) // versioned directories sort oldest -> newest
		return hits[len(hits)-1]
	}
	return ""
}

// DetectToolchain picks the best available toolchain for `target`.
func DetectToolchain(target Target) (Toolchain, error) {
	var tc Toolchain
	triple := target.Triple
	if triple == "" {
		triple = "x86-freestanding"
	}

	if cc := os.Getenv("TOS_CC"); cc != "" {
		tc.CC = strings.Fields(cc)
		if ld := os.Getenv("TOS_LD"); ld != "" {
			tc.LD = strings.Fields(ld)
		} else if p := findTool("ld.lld"); p != "" {
			tc.LD = []string{p}
		} else if p := findTool("i686-elf-ld"); p != "" {
			tc.LD = []string{p}
		} else {
			return tc, fmt.Errorf("TOS_CC is set but no linker was found (set TOS_LD)")
		}
		return tc, nil
	}

	if zig := findZig(); zig != "" {
		// zig cc is an all-in-one cross compiler + lld; it handles every
		// target triple we support, so prefer it.
		tc.CC = []string{zig, "cc", "--target=" + triple}
		tc.LD = []string{zig, "cc", "--target=" + triple}
		tc.UsesZig = true
		return tc, nil
	}

	if target.Name == "rv32" {
		// riscv32 has no zig here: drive clang for compilation and lld
		// directly for linking.  A compiler driver would add its own
		// compiler_rt, built with the hard-float ABI that refuses to mix
		// with our soft-float (ilp32) objects.
		clang := findTool("clang", "clang-19", "clang-18", "clang-17", "clang-16")
		lld := findTool("ld.lld", "ld.lld-19", "ld.lld-18", "ld.lld-17", "ld.lld-16")
		if clang != "" && lld != "" {
			tc.CC = []string{clang, "--target=" + triple}
			// Link with lld itself: a compiler driver would add its own
			// compiler_rt, built with the hard-float ABI that refuses to
			// mix with our soft-float (ilp32) objects.
			tc.LD = []string{lld, "-m", "elf32lriscv"}
			return tc, nil
		}
	}

	if target.Name == "x86" {
		clang := findTool("clang", "clang-19", "clang-18", "clang-17", "clang-16")
		lld := findTool("ld.lld", "ld.lld-19", "ld.lld-18", "ld.lld-17", "ld.lld-16")
		if clang != "" && lld != "" {
			// Drive lld through clang rather than calling it directly: the
			// link helpers pass driver-style flags (-Wl,..., -nostdlib,
			// -pie) that a bare ld.lld would reject.  lld must be named
			// explicitly because clang on MSYS2 defaults to GNU ld, which
			// has no elf_i386 emulation.
			tc.CC = []string{clang, "--target=i386-elf", "-m32"}
			tc.LD = []string{clang, "--target=i386-elf", "-m32", "-fuse-ld=lld"}
			return tc, nil
		}
	}

	gcc := findTool("i686-elf-gcc")
	gld := findTool("i686-elf-ld")
	if target.Name == "x86" && gcc != "" && gld != "" {
		tc.CC = []string{gcc}
		tc.LD = []string{gld}
		return tc, nil
	}

	if runtime.GOOS != "windows" {
		gcc = findTool("gcc")
		gld = findTool("ld")
		if gcc != "" && gld != "" {
			tc.CC = []string{gcc, "-m32"}
			tc.LD = []string{gld, "-m", "elf_i386"}
			return tc, nil
		}
	}

	return tc, fmt.Errorf(`no i386-elf toolchain found.

TinyOS needs clang + lld (MSYS2 UCRT64), Zig 0.13+, or i686-elf-gcc + i686-elf-ld.

With MSYS2 (recommended, UCRT64):

    pacman -S --needed mingw-w64-ucrt-x86_64-clang ^
                      mingw-w64-ucrt-x86_64-lld ^
                      mingw-w64-ucrt-x86_64-qemu

C:\msys64\ucrt64\bin is searched automatically; otherwise set TOS_BIN.

Zig is also supported (it bundles clang and lld):

    https://ziglang.org/download/
    zig cc is detected automatically (also under %%LOCALAPPDATA%%\Programs\zig)

You can also point TOS_CC / TOS_LD at a custom cross toolchain.`)
}
