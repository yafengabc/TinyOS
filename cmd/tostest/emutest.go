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

var badSigns = []string{"KERNEL PANIC", "System halted"}

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

// cmdEmutest implements `tostest emutest`, the Go port of scripts/esp-c3/
// emutest.py: a full functional pass on RV32EMU-C.
func cmdEmutest(args []string) int {
	_ = flag.NewFlagSet("emutest", flag.ExitOnError).Parse(args)
	emu := findEmu()
	if !fileExists(emu) {
		fmt.Fprintf(os.Stderr, "RV32EMU-C not found: %s (run 'tostest emu build')\n", emu)
		return 1
	}
	elf := filepath.Join(repoRoot(), "build", "kernel-esp32c3.elf")
	rd := filepath.Join(repoRoot(), "build", "ramdisk-esp32c3.bin")
	if !fileExists(elf) {
		fmt.Fprintf(os.Stderr, "build artifact missing: %s (run 'tos build -target esp32c3')\n", elf)
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
				grace := time.Now().Add(150 * time.Millisecond)
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

	checks := []struct {
		name string
		line string
		want []string
	}{
		// `ls` lists one level, so a directory shows as "bin/", not "bin/sh"
		// (this expectation predated the shell's directory support).  dev and
		// proc are the two namespaces devfs always exposes.
		{"ls", "ls", []string{"dev/", "proc/", "bin/", "readme.txt"}},
		{"cat readme.txt", "cat readme.txt", []string{"TinyOS"}},
		{"hello", "hello", []string{"hello"}},
		{"hello_printf", "hello_printf", []string{"hello"}},
		{"mem", "mem", []string{"heap demo"}},
		{"uptime", "uptime", []string{"uptime:", "pid="}},
		// wink is the window the task actually holds.  The shell's 8 KiB
		// image plus the 8 KiB stack and the 4 KiB guard rounds up to
		// 32 KiB - it used to reserve a fixed 64 KiB whatever it needed.
		// stk/sz is the stack high-water mark over the stack the task holds
		// right now: exec() fills the stack with 0xCC and ps scans up for the
		// first byte that changed, so the numerator is the only measurement
		// of how much of the stack is guesswork, and the denominator is the
		// size being judged (the default, a TOS_STACK() request, or what a
		// fault grew it to).
		{"ps", "ps", []string{"cpums wink stk/sz", "  32 "}},
		// Four frames of 2 KiB, parked at the bottom: the number ps reports
		// has to land in the eights.  A scanner that never looked, or that
		// counted frames the compiler dropped, would read 0.3.
		{"stk 4", "stk 4 3000 &", []string{"tos>"}},
		{"ps (stk)", "ps", []string{"  32    8.", "/16  stk"}},
		// Let the parked task exit before the window tests below: they fill
		// the user area exactly, so a leftover task would refuse the fifth
		// hold and the failure would look like a window bug.
		{"wait out stk", "hold 3", []string{"tos>"}},
		{"crash", "crash", []string{"illegal instruction", "tos>"}},
		{"source etc/rc", "source etc/rc", []string{"tos>"}},
		// Devices are files, so the table has to be enumerable and entry-able,
		// not just openable.  This is the only target tested end to end with
		// peripherals behind it, so it is the one place /dev is not empty.
		{"ls /dev", "ls /dev", []string{"gpio0", "gpio21"}},
		{"ls /proc", "ls /proc", []string{"uptime", "ps", "mem", "stack"}},
		// The record is taken when a task dies, not sampled: stk burned 8.3
		// KiB and exited during the wait above, and this still reports it.
		// That is the number a program is supposed to read before choosing
		// its own TOS_STACK(), so it has to survive the program it belongs
		// to.
		{"proc stack", "cat /proc/stack", []string{"high-water", "8.", "stk"}},
		// The other half of the stack story.  deep declares nothing, runs off
		// the default 8 KiB, and the kernel doubles the stack into the heap's
		// unused tail instead of killing it - so the program that could not
		// have known its depth survives, and the journal it wrote below the
		// break is intact, which is what says the growth stopped where it
		// should have.
		{"deep grows", "deep 6 100", []string{"stack grown to 16 KiB", "journal intact"}},
		{"deep (parked)", "deep 6 3000 &", []string{"tos>"}},
		{"ps (grown)", "ps", []string{"  32   12."}},
		{"wait out deep", "hold 3", []string{"tos>"}},
		// The heap can take the slack first, and then the stack has nowhere to
		// grow: 8 KiB of malloc moves the break to the ceiling, so the same
		// overflow is a real fault again and the task dies as it used to.
		// Sharing one region means the first claim wins - which is exactly
		// why TOS_STACK() still exists for a program that cannot risk it.
		{"deep, heap in the way", "deep 6 0 8", []string{"store access fault", "tos>"}},
		// A declared heap is a floor on the window, not a run-time
		// allocation: wheap's image is one page, but its TOS_HEAP(12 KiB)
		// note makes the window 32 KiB instead of the 16 a bare image of
		// this size would take (4 image + 12 heap + 8 stack + 4 guard).
		// The writes matter - malloc only moves the break, so a break that
		// ended up somewhere wrong would still "succeed" until the first
		// store, and the read-back is what says the region is real.
		{"wheap takes its declared heap", "wheap",
			[]string{"12 KiB declared, 12 KiB taken, pattern intact"}},
		{"wheap (parked)", "wheap 3000 &", []string{"tos>"}},
		{"ps (declared heap)", "ps", []string{"wheap", "  32 "}},
		{"wait out wheap", "hold 3", []string{"tos>"}},
		{"cd /dev", "cd /dev", []string{"/dev tos>"}},
		{"ls (in /dev)", "ls", []string{"gpio4"}},
		{"cd /", "cd /", []string{"tos>"}},
		// The image is built small (600 sectors = 300 KiB) and the kernel
		// grows it to the real partition on mount (ramdisk_init ->
		// lfs_fs_grow, PLAT_LFS_BLOCKS = 896 blocks).  df's total is the
		// only place the grown size shows; 307200 would mean the grow did
		// not happen and the FS is still living in the shipped image.
		{"df (grown)", "df", []string{"3670016"}},
		// dd writes 320 KiB = 80 blocks: the shipped image only has ~40
		// free ones, so allocation must spill into the grown tail and the
		// pattern must read back identical.  This is the end-to-end proof
		// that blocks beyond the image are usable, not just addressable.
		{"dd write (grown tail)", "dd of=/big.bin bs=4096 count=80",
			[]string{"wrote 327680 bytes"}},
		{"dd verify", "dd if=/big.bin bs=4096 count=80",
			[]string{"pattern verified"}},
		// unlink.  A directory is refused (lfs_remove would delete it
		// recursively and take /bin with it), a missing name is refused, and
		// a removed file is gone for good - the read side can no longer open
		// it, which is the only proof that the tree, not just the handle,
		// lost the entry.
		{"rm refuses a directory", "rm /bin", []string{"cannot remove /bin"}},
		{"rm missing file", "rm /nope", []string{"cannot remove /nope"}},
		{"rm", "rm /big.bin", []string{"tos>"}},
		{"dd after rm", "dd if=/big.bin", []string{"cannot open /big.bin"}},
		// Windows are sized to the image, so the user area packs: ten idle
		// holders at 16 KiB each plus the shell's 32 KiB fill all 192 KiB,
		// and the next exec has to be refused with a reason.  The holders
		// used to be 32 KiB because every task reserved a 16 KiB stack;
		// the default is 8 KiB now, so a 300-byte program gets a 16 KiB
		// window and the area holds twice as many.  The old fixed-slot
		// allocator could only say "out of task windows", which on a
		// machine with free memory reads like a leak; the
		// largest-free-block number is what tells a full area from a
		// fragmented one.
		{"hold 1", "hold 60 &", []string{"tos>"}},
		{"ps (small window)", "ps", []string{"  16 "}},
		{"hold 2", "hold 60 &", []string{"tos>"}},
		{"hold 3", "hold 60 &", []string{"tos>"}},
		{"hold 4", "hold 60 &", []string{"tos>"}},
		{"hold 5", "hold 60 &", []string{"tos>"}},
		{"hold 6", "hold 60 &", []string{"tos>"}},
		{"hold 7", "hold 60 &", []string{"tos>"}},
		{"hold 8", "hold 60 &", []string{"tos>"}},
		{"hold 9", "hold 60 &", []string{"tos>"}},
		{"hold 10", "hold 60 &", []string{"tos>"}},
		{"user area full", "hold 1", []string{"largest free block is 0 KiB"}},
	}
	for _, c := range checks {
		tstart := time.Now()
		out := send(c.line, 4*time.Second)
		ms := int64(time.Since(tstart) / time.Millisecond)
		fmt.Printf("---- %s (%d ms) ----\n", c.name, ms)
		fmt.Println(out)
		for _, w := range c.want {
			if !strings.Contains(out, w) {
				fmt.Printf("  WARN: expected %q not seen in output of %q\n", w, c.name)
			}
		}
	}

	// ---- reboot ----
	fmt.Println("---- reboot (expect software reset + banner again) ----")
	mark := s.mark()
	_ = s.sendLine("reboot", "\r\n")
	trb := time.Now()
	sawReset, sawPrompt2 := false, false
	end := time.Now().Add(30 * time.Second)
	for time.Now().Before(end) {
		b, more := s.read(200 * time.Millisecond)
		if b != nil {
			txt := s.text()
			if strings.Contains(txt, "software reset") || strings.Contains(txt, "rebooting") {
				sawReset = true
			}
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

	pass := sawReset && sawPrompt2
	fmt.Printf("==== EMU TEST %s (boot %d ms, reboot %d ms) ====\n",
		boolStr(pass), bootMs, rbMs)
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
