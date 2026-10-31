package main

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"

	"tinyos/internal/build"
)

func usage() {
	fmt.Print(`TinyOS build tool

usage:
  tos build [-v] [-target x86|rv32]
                            build build/tinyos.img (x86) or
                            build/kernel-rv32.elf (rv32)
  tos run   [-headless] [-debug] [-no-build] [-target x86|rv32]
                            build (unless -no-build) and boot in QEMU
  tos doctor                show detected toolchains and QEMU
  tos clean                 remove the build directory

environment:
  TOS_BIN   extra bin directories (separated by ;)
  TOS_CC    custom C compiler command line
  TOS_LD    custom linker command line
  TOS_QEMU  path to the QEMU binary for the selected target
  TOS_OPT   optimisation level, default -Oz (try -O2 or -Og when debugging)
`)
}

func main() {
	if len(os.Args) < 2 {
		usage()
		os.Exit(2)
	}

	root, err := build.FindRoot()
	if err != nil {
		fmt.Fprintln(os.Stderr, "tos:", err)
		os.Exit(1)
	}
	b := build.New(root)

	switch os.Args[1] {
	case "build":
		fs := flag.NewFlagSet("build", flag.ExitOnError)
		verbose := fs.Bool("v", false, "print full compiler command lines")
		target := fs.String("target", "x86", "build target: x86 or rv32")
		fs.Parse(os.Args[2:])
		b.Verbose = *verbose
		t, ok := build.Targets[*target]
		if !ok {
			fmt.Fprintf(os.Stderr, "unknown target %q (want x86 or rv32)\n", *target)
			os.Exit(2)
		}
		b.Target = t
		if err := b.Build(); err != nil {
			fmt.Fprintln(os.Stderr, "\ntos:", err)
			os.Exit(1)
		}
		if b.Target.Name == "x86" {
			fmt.Println("\nbuild ok -> build/tinyos.img")
		} else {
			fmt.Printf("\nbuild ok -> build/kernel-%s.elf\n", b.Target.Name)
		}

	case "run":
		fs := flag.NewFlagSet("run", flag.ExitOnError)
		headless := fs.Bool("headless", false, "no VGA window, console on the serial port")
		debug := fs.Bool("debug", false, "log interrupts / CPU resets to build/qemu.log")
		noBuild := fs.Bool("no-build", false, "do not rebuild before running")
		verbose := fs.Bool("v", false, "verbose build")
		target := fs.String("target", "x86", "build target: x86 or rv32")
		fs.Parse(os.Args[2:])
		b.Verbose = *verbose
		t, ok := build.Targets[*target]
		if !ok {
			fmt.Fprintf(os.Stderr, "unknown target %q (want x86 or rv32)\n", *target)
			os.Exit(2)
		}
		b.Target = t
		if !*noBuild {
			if err := b.Build(); err != nil {
				fmt.Fprintln(os.Stderr, "\ntos:", err)
				os.Exit(1)
			}
			fmt.Println()
		}
		b.QEMU = os.Getenv("TOS_QEMU")
		if err := b.Run(*headless, *debug); err != nil {
			fmt.Fprintln(os.Stderr, "\ntos:", err)
			os.Exit(1)
		}

	case "doctor":
		fmt.Printf("source tree : %s\n", root)
		for _, name := range []string{"x86", "rv32"} {
			t := build.Targets[name]
			tc, err := build.DetectToolchain(t)
			if err != nil {
				fmt.Printf("%-4s  : NOT FOUND\n", name)
				continue
			}
			fmt.Printf("%-4s  : %s\n", name, tc)
			version(tc.CC[0], "--version")
		}
		for _, name := range []string{"x86", "rv32"} {
			b := build.New(root)
			b.Target = build.Targets[name]
			qemu := b.FindQEMU()
			if qemu == "" {
				fmt.Printf("qemu %-4s: NOT FOUND (pacman -S mingw-w64-ucrt-x86_64-qemu)\n", name)
			} else {
				fmt.Printf("qemu %-4s: %s\n", name, qemu)
			}
		}

	case "clean":
		if err := os.RemoveAll(filepath.Join(root, "build")); err != nil {
			fmt.Fprintln(os.Stderr, "tos:", err)
			os.Exit(1)
		}
		fmt.Println("clean ok")

	case "-h", "--help", "help":
		usage()

	default:
		fmt.Fprintf(os.Stderr, "tos: unknown command %q\n\n", os.Args[1])
		usage()
		os.Exit(2)
	}
}

func version(tool, arg string) {
	out, err := exec.Command(tool, arg).Output()
	if err != nil {
		return
	}
	line := ""
	for _, c := range string(out) {
		if c == '\n' || c == '\r' {
			break
		}
		line += string(c)
	}
	fmt.Printf("              %s\n", line)
}
