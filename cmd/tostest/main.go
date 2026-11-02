// tostest - the TinyOS test harness.
//
// Everything that used to be a Python or PowerShell script in scripts/ lives
// here as Go subcommands, so a fresh checkout needs only the C toolchain and
// Go: no Python, no PowerShell.  The one exception is the esp32c3 real-hardware
// bring-up in scripts/esp-c3/, which stays in Python because esptool/pyserial
// are the ESP ecosystem standard (see the README).
//
//	tostest smoke   [-target x86|rv32|arm] [-commands "ls,cat ..."]
//	tostest compat  [-targets x86,rv32,arm]
//	tostest emu     build|test     (rv32emu-c submodule)
//	tostest emutest
//	tostest bench   [-qemu [port]] [payload]
package main

import (
	"fmt"
	"os"
)

const usage = `tostest - TinyOS test harness

usage:
  tostest smoke  [-target x86|rv32|arm] [-port 5566]
                 [-commands "ls,cat readme.txt,hello alpha"]
                 [-commandwait-ms 700] [-allow-reboot]
                   boot a build in QEMU over a TCP serial port and drive it
  tostest compat [-targets x86,rv32] [-port 5570] [-no-build] [-verbose]
                   cross-target compatibility check (same commands, same output);
                   add arm explicitly once its exec HardFault is fixed
  tostest emu    build|test
                   build the rv32emu-c submodule into build/rv32emu-c.exe, or
                   boot the esp32c3 kernel on it and poke the shell
  tostest emutest
                   full functional test on RV32EMU-C (boot, user programs,
                   crash isolation, source, reboot)
  tostest bench  [-qemu [port]] [payload]
                   time the guest bench payload on RV32EMU-C (default) or a
                   QEMU TCP serial port

environment:
  TOS_EMU     overrides the emulator binary used by emu/emutest/bench
  TOS_QEMU    overrides the QEMU binary used by smoke/compat/bench -qemu
  TOS_BIN     extra bin directories (separated by ; or :)
`

func main() {
	if len(os.Args) < 2 {
		fmt.Fprint(os.Stderr, usage)
		os.Exit(2)
	}
	rc := 2
	switch os.Args[1] {
	case "smoke":
		rc = cmdSmoke(os.Args[2:])
	case "compat":
		rc = cmdCompat(os.Args[2:])
	case "emu":
		rc = cmdEmu(os.Args[2:])
	case "emutest":
		rc = cmdEmutest(os.Args[2:])
	case "bench":
		rc = cmdBench(os.Args[2:])
	case "help", "-h", "--help":
		fmt.Print(usage)
		rc = 0
	default:
		fmt.Fprintf(os.Stderr, "unknown command %q\n\n", os.Args[1])
		fmt.Fprint(os.Stderr, usage)
	}
	os.Exit(rc)
}
