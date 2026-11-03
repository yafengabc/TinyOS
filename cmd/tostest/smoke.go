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

// smokeOpts drives one QEMU session over a TCP serial port.  It is the Go
// port of scripts/smoke.ps1 and is shared by the `smoke` and `compat`
// subcommands.
type smokeOpts struct {
	target      string
	image       string
	qemu        string
	commands    []string
	port        int
	bootWait    time.Duration
	cmdWait     time.Duration
	allowReboot bool
}

func qemuArgs(target string, port int, image string, allowReboot bool) []string {
	var args []string
	switch target {
	case "rv32", "rv32smp":
		// No firmware: the kernel ELF is loaded at 0x80000000 and starts in
		// machine mode, so it brings its own trap vector and device setup.
		args = []string{
			"-machine", "virt",
			"-m", "256M",
			"-bios", "none",
			"-kernel", image,
			"-display", "none",
			"-chardev", fmt.Sprintf("socket,id=tosserial,host=127.0.0.1,port=%d,server=on,wait=on", port),
			"-serial", "chardev:tosserial",
			"-monitor", "none",
		}
		// rv32smp is the two-hart build (internal/build: CPUs: 2); every
		// other target is a uniprocessor and must not be given -smp.
		if target == "rv32smp" {
			args = append(args, "-smp", "2")
		}
	case "arm":
		// mps2-an385 (Cortex-M3): the kernel ELF is mapped into SSRAM at 0,
		// its vector table is the reset entry and UART0 is the only serial.
		args = []string{
			"-machine", "mps2-an385",
			"-kernel", image,
			"-display", "none",
			"-chardev", fmt.Sprintf("socket,id=tosserial,host=127.0.0.1,port=%d,server=on,wait=on", port),
			"-serial", "chardev:tosserial",
			"-monitor", "none",
		}
	default:
		args = []string{
			"-m", "128M",
			"-drive", fmt.Sprintf("file=%s,format=raw,if=ide", image),
			"-boot", "c",
			"-display", "none",
			"-chardev", fmt.Sprintf("socket,id=tosserial,host=127.0.0.1,port=%d,server=on,wait=on", port),
			"-serial", "chardev:tosserial",
			"-monitor", "none",
		}
	}
	// A guest reset normally ends the run; -allow-reboot keeps QEMU alive.
	if !allowReboot && target != "x86" {
		args = append(args, "-no-reboot")
	}
	return args
}

// runSmoke boots target in QEMU, feeds the commands over the TCP serial port
// and returns the transcript (guest output plus, when stderr was captured, a
// "--- qemu stderr ---" footer).  The caller prints/filters it.
func runSmoke(opts smokeOpts) (string, error) {
	name := qemuName(opts.target)
	qemu := opts.qemu
	if qemu == "" {
		qemu = findQEMU(name)
	}
	if qemu == "" {
		return "", fmt.Errorf("%s not found (set TOS_QEMU or TOS_BIN)", name)
	}

	image := opts.image
	if image == "" {
		switch opts.target {
		case "rv32", "rv32smp":
			image = filepath.Join(repoRoot(), "build", "kernel-"+opts.target+".elf")
		case "arm":
			image = filepath.Join(repoRoot(), "build", "kernel-arm.elf")
		default:
			image = filepath.Join(repoRoot(), "build", "tinyos.img")
		}
	}
	if _, err := os.Stat(image); err != nil {
		return "", fmt.Errorf("build artifact not found: %s (run 'tos build -target %s')", image, opts.target)
	}

	args := qemuArgs(opts.target, opts.port, image, opts.allowReboot)
	cmd := exec.Command(qemu, args...)
	s, err := startSession(cmd, false)
	if err != nil {
		return "", fmt.Errorf("starting %s: %w", qemu, err)
	}

	client, err := dialSerial(opts.port, 10*time.Second)
	if err != nil {
		s.close()
		return "", fmt.Errorf("could not connect to the QEMU serial port on %s", fmtSockAddr(opts.port))
	}

	client.drain(500 * time.Millisecond)
	// 60 s: the rv32 kernel takes seconds to walk the filesystem under TCG.
	ok, _ := client.waitFor([]byte("tos>"), 60*time.Second)
	if !ok {
		client.close()
		s.close()
		return "", fmt.Errorf("shell prompt never appeared:\n%s", client.text())
	}

	for _, c := range opts.commands {
		_ = client.send(c)
		client.drain(opts.cmdWait)
		// A dead shell must not read as a pass: the prompt had appeared once,
		// no panic was printed, and later commands went to a machine that was
		// no longer listening.  `exit` is the one command whose job is to end
		// init, so it is exempt.
		if c != "exit" && strings.Contains(client.text(), "init process exited") {
			client.close()
			s.close()
			return client.text(), fmt.Errorf("SMOKE TEST FAILED: the shell died during %q", c)
		}
	}

	client.drain(300 * time.Millisecond)
	client.close()
	s.close()

	out := client.text()
	if stderr := s.stderr.String(); stderr != "" {
		out += "\n--- qemu stderr ---\n" + stderr
	}
	if strings.Contains(out, "KERNEL PANIC") || strings.Contains(out, "KERNEL EXCEPTION") {
		return out, fmt.Errorf("SMOKE TEST FAILED: kernel panic in transcript")
	}
	if !strings.Contains(out, "tos>") {
		return out, fmt.Errorf("SMOKE TEST FAILED: shell prompt never appeared")
	}
	return out, nil
}

func cmdSmoke(args []string) int {
	fs := flag.NewFlagSet("smoke", flag.ExitOnError)
	target := fs.String("target", "x86", "x86|rv32|arm")
	image := fs.String("image", "", "explicit boot image")
	qemu := fs.String("qemu", "", "explicit QEMU binary")
	port := fs.Int("port", 5566, "TCP serial port")
	bootWait := fs.Int("bootwait-ms", 3000, "unused, kept for compatibility")
	cmdWait := fs.Int("commandwait-ms", 700, "per-command output window")
	allowReboot := fs.Bool("allow-reboot", false, "keep QEMU alive across a guest reset")
	commands := fs.String("commands", "", "comma-separated commands (defaults to the standard list)")
	_ = bootWait
	_ = fs.Parse(args)

	cmdList := strings.Split(*commands, ",")
	if *commands == "" {
		cmdList = []string{
			"ls", "cat readme.txt", "hello alpha beta", "hello_printf alpha beta",
			"reloc", "run spin &", "ps", "wfile demo.txt hello from the filesystem",
			"cat demo.txt", "mem", "run prot heap", "crash", "uptime", "exit",
		}
	}

	transcript, err := runSmoke(smokeOpts{
		target:      *target,
		image:       *image,
		qemu:        *qemu,
		commands:    cmdList,
		port:        *port,
		bootWait:    time.Duration(*bootWait) * time.Millisecond,
		cmdWait:     time.Duration(*cmdWait) * time.Millisecond,
		allowReboot: *allowReboot,
	})
	fmt.Println(transcript)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	fmt.Println("SMOKE TEST OK")
	return 0
}
