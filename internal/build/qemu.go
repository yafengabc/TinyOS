package build

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

// FindQEMU exposes the QEMU lookup to the CLI.
func (b *Builder) FindQEMU() string { return findQEMU() }

// Run launches QEMU with the freshly built disk image.
func (b *Builder) Run(headless, debug bool) error {
	img := filepath.Join(b.BuildDir, "tinyos.img")
	if _, err := os.Stat(img); err != nil {
		return fmt.Errorf("no disk image, run 'tos build' first")
	}

	qemu := b.QEMU
	if qemu == "" {
		qemu = findQEMU()
	}
	if qemu == "" {
		return fmt.Errorf(`qemu-system-i386 not found.

Install it with MSYS2:

    pacman -S mingw-w64-ucrt-x86_64-qemu

and add C:\msys64\ucrt64\bin to PATH, or set TOS_BIN/TOS_QEMU.`)
	}

	args := []string{
		"-m", "128M",
		"-drive", "file=" + img + ",format=raw,if=ide",
		"-boot", "c",
		"-no-reboot",
	}

	if headless {
		args = append(args,
			"-display", "none",
			"-monitor", "none",
			"-serial", "stdio",
		)
	} else {
		args = append(args,
			"-serial", "file:"+filepath.Join(b.BuildDir, "serial.log"),
		)
	}

	if debug {
		args = append(args,
			"-d", "int,cpu_reset,guest_errors",
			"-D", filepath.Join(b.BuildDir, "qemu.log"),
		)
	}

	fmt.Printf("QEMU: %s\n", qemu)
	fmt.Printf("image: %s\n\n", img)

	cmd := exec.Command(qemu, args...)
	cmd.Stdin = os.Stdin
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}
