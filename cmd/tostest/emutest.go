package main

import (
	"bytes"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

var badSigns = []string{"KERNEL PANIC", "System halted", "[bus] unmapped"}

func hasBad(text string) bool {
	for _, b := range badSigns {
		if strings.Contains(text, b) {
			return true
		}
	}
	return false
}

func bootDone(text string) bool {
	return strings.Contains(text, "TinyOS shell") && strings.Contains(text, "tos> ")
}

// checkWait bounds one command.  The Go rv32emu counts guest time in APB
// cycles (1 instruction = 1 cycle at 80 MHz) rather than in wall time, and it
// executes roughly a quarter of that rate, so a guest `sleep 500` takes about
// two seconds here and the fixture's `hold 3` (a three-second park used to let
// a sibling exit) takes about twelve.  Four seconds, the old cap, cut the
// command off and every later check then looked at the previous command's
// output.
const checkWait = 40 * time.Second

type emuCheck struct {
	name string
	line string
	want []string
}

// cmdEmutest implements `tostest emutest`: a functional pass on the C3 kernel,
// with the console wired to the Go rv32emu in this tree.
func cmdEmutest(args []string) int {
	_ = flag.NewFlagSet("emutest", flag.ExitOnError).Parse(args)
	emu := findEmu()
	if !fileExists(emu) {
		fmt.Fprintf(os.Stderr, "rv32emu not found: %s (run 'tostest emu build')\n", emu)
		return 1
	}
	elf := filepath.Join(repoRoot(), "build", "kernel-esp32c3-generic.elf")
	rd := filepath.Join(repoRoot(), "build", "tinyos-ramdisk-esp32c3-generic.bin")
	if !fileExists(elf) {
		fmt.Fprintf(os.Stderr, "build artifact missing: %s (run 'tos build -target esp32c3-generic')\n", elf)
		return 1
	}

	s, err := startEmulator(emu, elf, rd)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}
	defer s.close()

	// ---- boot ----
	t0 := time.Now()
	ok, _ := s.waitForText(bootDone, 60*time.Second)
	bootMs := int64(time.Since(t0) / time.Millisecond)
	bootlog := s.text()
	fmt.Printf("==== boot (%d ms wall) ====\n", bootMs)
	fmt.Println(bootlog)
	if !ok {
		fmt.Fprintln(os.Stderr, "EMU TEST FAILED: no shell prompt")
		return 1
	}
	if hasBad(bootlog) {
		fmt.Fprintln(os.Stderr, "EMU TEST FAILED: kernel panic during boot")
		return 1
	}

	// ---- command checks ----
	send := func(line string, wait time.Duration) string {
		_ = s.sendLine(line, "\r\n")
		var out []byte
		tend := time.Now().Add(wait)
		for time.Now().Before(tend) {
			b, more := s.read(200 * time.Millisecond)
			if b != nil {
				out = append(out, b...)
			}
			if !more {
				break
			}
			if bytes.Contains(out, []byte("tos> ")) {
				// The shell answers the CR and the LF after it with a prompt
				// each; drain the trailing one so the next command is not
				// measured against a leftover prompt.
				grace := time.Now().Add(300 * time.Millisecond)
				for time.Now().Before(grace) {
					b2, more2 := s.read(50 * time.Millisecond)
					if b2 != nil {
						out = append(out, b2...)
					}
					if !more2 {
						break
					}
				}
				break
			}
		}
		return string(out)
	}

	// Cells quoted below are what this image prints; the numbers that came
	// from the rv32 QEMU target (32 KiB stack, 16 processes...) are noted
	// where the C3 differs, because the C3 is the small one.
	checks := []emuCheck{
		// `ls` lists one level, so a directory shows as "bin/", not "bin/sh".
		{"ls", "ls", []string{"dev/", "proc/", "bin/", "readme.txt"}},
		{"cat readme.txt", "cat readme.txt", []string{"TinyOS"}},
		{"hello", "hello", []string{"hello"}},
		{"hello_printf", "hello_printf", []string{"hello"}},
		{"mem", "mem", []string{"heap demo"}},
		{"uptime", "uptime", []string{"uptime:", "pid="}},
		// A program on the console, a builtin inside a script: this covers
		// exec + ST_WAIT_SLEEP and keeps ^C able to find a foreground job.
		{"sleep", "sleep 500", []string{"tos>"}},
		// The kernel refuses instead of clamping: both paths say the limit.
		{"sleep too long", "sleep 99999999", []string{"max 3600000"}},
		// The C3 window is 16 KiB (4 image + 4 stack + 4 guard -> 16 after the
		// grain); the rv32 QEMU board gives the shell 32.  stk/sz is the stack
		// high-water mark over the stack the task holds right now.
		{"ps", "ps", []string{"cpums wink stk/sz", "  16 "}},
		// Four frames of 2 KiB in a 16 KiB TOS_STACK: the denominator is 16
		// and the numerator lands in the eights.  The window is 24 KiB.
		{"stk 4", "stk 4 3000 &", []string{"tos>"}},
		{"ps (stk)", "ps", []string{"24    8.", "/16  stk"}},
		// The machine view of the same rows: one line per thread, keyed by
		// (tgid, tid).  A kernel thread has no process and is not here.
		{"threads", "cat /proc/threads", []string{"tgid  tid", "msh"}},
		// Let the parked task exit before the window tests below.
		{"wait out stk", "hold 3", []string{"tos>"}},
		{"crash", "crash", []string{"illegal instruction", "tos>"}},
		{"source etc/rc", "source etc/rc", []string{"tos>"}},
		// The script fixture exercises the whole language in one pass:
		// $(...) over a pipe, if/else, while/incr/break, a builtin sleep,
		// a program call and a two-program pipeline.  It runs under /bin/sh,
		// the script interpreter: msh is the default prompt and deliberately
		// drops if/while/$(...), so `source etc/demo.sh` in msh executed each
		// line as a program (and printed "exec: '/bin/if' not found"), which
		// is what this check used to do - the fixture was never the problem.
		{"script", "sh etc/demo.sh", []string{"[script] if-ok",
			"[script] capture-ok", "[script] loop 2", "[script] after-loop i=3",
			"[script] break-at-2", "[script] done"}},
		// Devices are files, so the table has to be enumerable and entry-able,
		// not just openable.  The C3 exposes one multiplexed `gpio` node
		// rather than the per-pin gpio0..gpio21 the check used to name.
		{"ls /dev", "ls /dev", []string{"rgb0", "gpio", "adc"}},
		{"ls /proc", "ls /proc", []string{"uptime", "ps", "mem", "stack"}},
		// The record is taken when a task dies, not sampled: stk burned 8.3
		// KiB and exited during the wait above, and this still reports it.
		{"proc stack", "cat /proc/stack", []string{"high-water", "8.", "stk"}},
		// The C3's default stack is 4 KiB and the window keeps one growth
		// step, so `deep 3` (6 KiB) is the run that survives: it doubles to
		// 8 KiB, and the journal below the break is intact, which is what
		// says the growth stopped where it should have.  `deep 6` is the
		// other side of the same rule and is checked further down.
		{"deep grows", "deep 3 100", []string{"stack grown to 8 KiB", "journal intact"}},
		{"deep (parked)", "deep 3 3000 &", []string{"tos>"}},
		{"ps (grown)", "ps", []string{"24    6.", "deep"}},
		{"wait out deep", "hold 3", []string{"tos>"}},
		// The heap gets there first: 8 KiB of malloc moves the break to the
		// ceiling, so the same overflow is a real fault again and the task
		// dies as it used to - the software guard catches it before the store.
		{"deep, heap in the way", "deep 6 0 8",
			[]string{"malloc stopped", "stack overflow", "killing"}},
		// A declared heap is a floor on the window: wheap's image is one page,
		// but its TOS_HEAP(12 KiB) note makes the window 32 KiB (4 image +
		// 12 heap + 8 stack + 4 guard).  The read-back is what says the region
		// is real - malloc alone only moves the break.
		{"wheap takes its declared heap", "wheap",
			[]string{"12 KiB declared, 12 KiB taken, pattern intact"}},
		{"wheap (parked)", "wheap 3000 &", []string{"tos>"}},
		{"ps (declared heap)", "ps", []string{"wheap", "  32 "}},
		{"wait out wheap", "hold 3", []string{"tos>"}},
		// The prompt no longer carries the cwd ("/dev tos>" was from when it
		// did); the next check, run from this directory, is what proves the
		// chdir worked.
		{"cd /dev", "cd /dev", []string{"tos>"}},
		{"ls (in /dev)", "ls", []string{"rgb0", "gpio"}},
		{"cd /", "cd /", []string{"tos>"}},
		// The image is built small and the kernel grows it to the real
		// partition on mount (ramdisk_init -> lfs_fs_grow).  df's total is the
		// only place the grown size shows; the shipped size would mean the
		// grow did not happen.
		{"df (grown)", "df", []string{"3670016"}},
		// dd writes 320 KiB = 80 blocks: allocation must spill into the grown
		// tail and the pattern must read back identical.
		{"dd write (grown tail)", "dd of=/big.bin bs=4096 count=80",
			[]string{"wrote 327680 bytes"}},
		{"dd verify", "dd if=/big.bin bs=4096 count=80",
			[]string{"pattern verified"}},
		// unlink.  A directory is refused, a missing name is refused, and a
		// removed file is gone for good.
		{"rm refuses a directory", "rm /bin", []string{"cannot remove /bin"}},
		{"rm missing file", "rm /nope", []string{"cannot remove /nope"}},
		{"rm", "rm /big.bin", []string{"tos>"}},
		{"dd after rm", "dd if=/big.bin", []string{"cannot open /big.bin"}},
	}

	// The process-table fill runs after the reboot: with max_procs = 8 (the
	// C3's RAM budget) shell + seven holders is the whole table, so the
	// eighth exec is refused by the table, not by the window allocator.  The
	// window-exhaustion path ("largest free block is 0 KiB") is therefore not
	// reachable on this image; it needs a target with room for ten windows.
	holdChecks := []emuCheck{
		{"hold 1", "hold 60 &", []string{"tos>"}},
		{"ps (small window)", "ps", []string{"  16 ", "hold"}},
		{"hold 2", "hold 60 &", []string{"tos>"}},
		{"hold 3", "hold 60 &", []string{"tos>"}},
		{"hold 4", "hold 60 &", []string{"tos>"}},
		{"hold 5", "hold 60 &", []string{"tos>"}},
		{"hold 6", "hold 60 &", []string{"tos>"}},
		{"hold 7", "hold 60 &", []string{"tos>"}},
		// The kernel says which table is full; a slot that never got a
		// process entry must not walk its absent descriptor table either -
		// that is the "[bus] unmapped" this run now fails on.
		{"process table full", "hold 1", []string{"process or thread table full"}},
	}

	run := func(list []emuCheck) int {
		missed := 0
		for _, c := range list {
			tstart := time.Now()
			out := send(c.line, checkWait)
			ms := int64(time.Since(tstart) / time.Millisecond)
			fmt.Printf("---- %s (%d ms) ----\n", c.name, ms)
			fmt.Println(out)
			for _, w := range c.want {
				if !strings.Contains(out, w) {
					fmt.Printf("  WARN: expected %q not seen in output of %q\n", w, c.name)
					missed++
				}
			}
		}
		return missed
	}

	missed := run(checks)

	// ---- reboot ----
	// Before the table-fill block on purpose: after it the table is full and
	// reboot, itself a /bin program, could not be spawned - which is how the
	// old run ended with "reboot observed=false" and told nobody why.
	fmt.Println("---- reboot (expect software reset + banner again) ----")
	mark := s.mark()
	_ = s.sendLine("reboot", "\r\n")
	trb := time.Now()
	sawReset, sawPrompt2 := false, false
	end := time.Now().Add(30 * time.Second)
	for time.Now().Before(end) {
		b, more := s.read(200 * time.Millisecond)
		if b != nil {
			txt := s.sinceMark(mark)
			if strings.Contains(txt, "software reset") || strings.Contains(txt, "rebooting") {
				sawReset = true
			}
			// A prompt only counts after the reset marker: the first boot's
			// prompt is still in the session buffer and used to make this
			// look successful before the guest had rebooted at all.
			if sawReset && strings.Contains(txt, "tos> ") {
				sawPrompt2 = true
				break
			}
		}
		if !more {
			break
		}
	}
	rbMs := int64(time.Since(trb) / time.Millisecond)
	rebuf := s.sinceMark(mark)
	fmt.Println(rebuf)
	fmt.Printf("  reboot observed=%v, second prompt=%v (%d ms)\n", sawReset, sawPrompt2, rbMs)
	if hasBad(rebuf) {
		fmt.Fprintln(os.Stderr, "EMU TEST FAILED: kernel panic after reboot")
		return 1
	}

	missed += run(holdChecks)

	// The whole run is scanned once more: a wild pointer can print on the
	// host (where the emulator notices an unmapped access) rather than in the
	// guest stream, and it may surface while another command is timing.
	if hasBad(s.text()) {
		fmt.Fprintln(os.Stderr, "EMU TEST FAILED: unmapped access or panic in the session")
		return 1
	}

	pass := sawReset && sawPrompt2 && missed == 0
	fmt.Printf("==== EMU TEST %s (boot %d ms, reboot %d ms, %d missing) ====\n",
		boolStr(pass), bootMs, rbMs, missed)
	if !pass {
		return 1
	}
	return 0
}

func boolStr(b bool) string {
	if b {
		return "OK"
	}
	return "FAILED"
}
