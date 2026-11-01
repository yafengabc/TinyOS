package build

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
)

// FindQEMU exposes the QEMU lookup to the CLI.
func (b *Builder) FindQEMU() string { return findQEMU(b.Target) }

// qemuBinary is the emulator that matches a build target.
func qemuBinary(t Target) string {
	switch t.Name {
	case "rv32":
		return "qemu-system-riscv32"
	case "arm":
		return "qemu-system-arm"
	}
	return "qemu-system-i386"
}

func findQEMU(t Target) string {
	name := qemuBinary(t)
	return findTool(name, name+"w")
}

// Run launches QEMU with the freshly built image.
func (b *Builder) Run(headless, debug bool) error {
	qemu := b.QEMU
	if qemu == "" {
		qemu = findQEMU(b.Target)
	}
	if qemu == "" {
		return fmt.Errorf(`%s not found.

Install it with MSYS2:

    pacman -S mingw-w64-ucrt-x86_64-qemu

and add C:\msys64\ucrt64\bin to PATH, or set TOS_BIN/TOS_QEMU.`, qemuBinary(b.Target))
	}

	var args []string
	if b.Target.Name == "rv32" || b.Target.Name == "arm" {
		// Both targets boot a raw kernel ELF instead of a disk image: the
		// loader places the image at its link address and the kernel brings
		// its own entry point, vector table and device setup.
		kernel := filepath.Join(b.BuildDir, "kernel-"+b.Target.Name+".elf")
		if _, err := os.Stat(kernel); err != nil {
			return fmt.Errorf("no kernel ELF, run 'tos build -target %s' first", b.Target.Name)
		}
		if b.Target.Name == "arm" {
			// mps2-an385 is a Cortex-M3 (SSRAM at 0, 4 MiB SRAM, CMSDK UART0).
			args = []string{
				"-machine", "mps2-an385",
				"-kernel", kernel,
			}
		} else {
			args = []string{
				"-machine", "virt",
				"-m", "256M",
				"-bios", "none",
				"-kernel", kernel,
			}
		}
		if headless {
			args = append(args, "-display", "none", "-monitor", "none", "-serial", "stdio")
		} else {
			args = append(args, "-serial", "file:"+filepath.Join(b.BuildDir, "serial.log"))
		}
		if debug {
			args = append(args,
				"-d", "int,cpu_reset,guest_errors",
				"-D", filepath.Join(b.BuildDir, "qemu.log"),
			)
		}
		fmt.Printf("QEMU: %s\n", qemu)
		fmt.Printf("kernel: %s\n\n", kernel)

		cmd := exec.Command(qemu, args...)
		cmd.Stdin = os.Stdin
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		return cmd.Run()
	}

	img := filepath.Join(b.BuildDir, "tinyos.img")
	if _, err := os.Stat(img); err != nil {
		return fmt.Errorf("no disk image, run 'tos build' first")
	}

	args = []string{
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
