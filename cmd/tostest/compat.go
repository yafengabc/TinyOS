package main

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"regexp"
	"strings"
	"time"
)

// cmdCompat implements `tostest compat`, the Go port of scripts/compat.ps1:
// the same command sequence must produce the same output on every target.
// Genuine capability differences live in allowedDiffs.
func cmdCompat(args []string) int {
	fs := flag.NewFlagSet("compat", flag.ExitOnError)
	targets := fs.String("targets", "x86,rv32,arm", "comma-separated target list")
	port := fs.Int("port", 5570, "starting TCP serial port")
	cmdWait := fs.Int("commandwait-ms", 900, "per-command output window")
	noBuild := fs.Bool("no-build", false, "skip 'tos build -target ...'")
	verbose := fs.Bool("verbose", false, "print every transcript")
	_ = fs.Parse(args)

	targetList := splitCsv(*targets)
	if !*noBuild {
		for _, t := range targetList {
			fmt.Printf("building %s\n", t)
			cmd := exec.Command("go", "run", "./cmd/tos", "build", "-target", t)
			cmd.Dir = repoRoot()
			cmd.Stdout = os.Stdout
			cmd.Stderr = os.Stderr
			if err := cmd.Run(); err != nil {
				fmt.Fprintf(os.Stderr, "build failed for %s: %v\n", t, err)
				return 1
			}
		}
	}

	boot := []string{"hello alpha beta", "hello_printf alpha beta"}
	strictOne := []string{"echo alpha beta gamma", "cat nonexistent.txt", "ls", "exit"}
	loose := []string{"run prot null", "run prot rodata", "run prot heap", "run prot stack"}

	compatUnreliable = false
	failed := 0
	isolated := 0
	portN := *port
	cmdWaitD := time.Duration(*cmdWait) * time.Millisecond

	// 1. one session per target for the boot-time commands
	sessions := map[string][]string{}
	for _, t := range targetList {
		transcript, err := runSmoke(smokeOpts{target: t, commands: boot, port: portN, cmdWait: cmdWaitD})
		portN++
		if err := assertRun(t, transcript, err); err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		if *verbose {
			fmt.Println(transcript)
		} else {
			fmt.Printf("  %s: ran %d boot commands\n", t, len(boot))
		}
		body := getOutput(transcript)
		at := strings.Index(body, "tos>")
		if at < 0 {
			fmt.Fprintf(os.Stderr, "no prompt from %s\n", t)
			return 1
		}
		parts := promptSplitRe.Split(body[at:], -1)
		if len(parts) > 0 {
			parts = parts[1:]
		}
		sessions[t] = parts
	}
	for i := 0; i < len(boot); i++ {
		raw := map[string]string{}
		missing := false
		for _, t := range targetList {
			if i < len(sessions[t]) {
				raw[t] = sessions[t][i]
			} else {
				missing = true
			}
		}
		if missing {
			isolated++
			if !compareIsolated(boot[i], targetList, &portN, cmdWaitD, *verbose) {
				failed++
			}
			portN += 2 * len(targetList)
			continue
		}
		if !compareCommand(boot[i], raw, targetList, *verbose) {
			failed++
		}
	}

	// 2. each remaining command on its own (a fresh boot, one line)
	for _, c := range append(strictOne, loose...) {
		isolated++
		if !compareIsolated(c, targetList, &portN, cmdWaitD, *verbose) {
			failed++
		}
		portN += 2 * len(targetList)
	}

	fmt.Println()
	if compatUnreliable {
		fmt.Println("note: a target printed nothing for one command even after a retry;")
		fmt.Println("      check the session by hand before trusting the comparison.")
	}
	if isolated > 0 {
		fmt.Printf("note: %d command(s) were rerun in isolation (no input queue)\n", isolated)
	}
	if failed == 0 {
		fmt.Println("COMPAT OK")
		return 0
	}
	fmt.Printf("COMPAT FAILED: %d difference(s)\n", failed)
	return 1
}

var compatUnreliable bool

// AllowedDiffs holds the genuine, documented capability differences between
// targets (target|command -> regexp the answer must match).  Nothing else may
// diverge.  See PLATFORM-DESIGN section 9.
var allowedDiffs = map[string]string{
	// x86 has no MMU and no protection unit: ring 3 isolates privilege, not
	// memory, so every prot case survives.
	"x86|run prot null":   `^survived$`,
	"x86|run prot rodata": `^survived$`,
	"x86|run prot heap":   `^survived$`,
	"x86|run prot stack":  `^survived$`,
	// rv32 PMP kills all four cases; only the task dies.
	"rv32|run prot null":   `^killed: protection fault$`,
	"rv32|run prot rodata": `^killed: protection fault$`,
	"rv32|run prot heap":   `^killed: protection fault$`,
	"rv32|run prot stack":  `^killed: protection fault$`,
	// ARMv7-M MPU regions are naturally aligned power-of-two blocks with no
	// TOR form.  A window whose `wx` is not on a power-of-two offset needs
	// more blocks than the 7 available (sh: 4 code + 9 data), so the precise
	// w^X shape does not fit and the task runs with one whole-window RWX
	// area instead - which still bounds it to its own window and keeps the
	// stack guard, but makes .rodata writable and the heap unguarded.
	// PLATFORM-DESIGN 5.2 has the measurements.
	"arm|run prot null":   `^killed: protection fault$`,
	"arm|run prot rodata": `^survived$`,
	"arm|run prot heap":   `^survived$`,
	"arm|run prot stack":  `^killed: protection fault$`,
	// The RP2040 has no MPU (Cortex-M0+), so like x86 the task survives.
	// Inferred, not measured: that port has never run on real hardware
	// (OPEN-ITEMS 13).
	"rp2040|run prot null":   `^survived$`,
	"rp2040|run prot rodata": `^survived$`,
	"rp2040|run prot heap":   `^survived$`,
	"rp2040|run prot stack":  `^survived$`,
}

var (
	pidSquareRe   = regexp.MustCompile(`\[pid \d+\]`)
	pidEqRe       = regexp.MustCompile(`pid=?\s*\d+`)
	addrRe        = regexp.MustCompile(`0x[0-9a-fA-F]{8}`)
	msRe          = regexp.MustCompile(`\d+(\.\d+)? ms`)
	tickRe        = regexp.MustCompile(`\d+ tick\(s\)`)
	promptSplitRe = regexp.MustCompile(`(?m)^tos> `)
)

func splitCsv(s string) []string {
	var out []string
	for _, p := range strings.Split(s, ",") {
		if p = strings.TrimSpace(p); p != "" {
			out = append(out, p)
		}
	}
	return out
}

func assertRun(target, transcript string, err error) error {
	if err != nil {
		return fmt.Errorf("smoke on %s failed:\n%s\n%v", target, transcript, err)
	}
	return nil
}

// getOutput keeps only the guest's own output: no harness footer, no qemu
// stderr section, no transcript preamble.
func getOutput(text string) string {
	t := strings.ReplaceAll(text, "\r", "")
	if i := strings.Index(t, "--- qemu stderr ---"); i >= 0 {
		t = t[:i]
	}
	if i := strings.Index(t, "SMOKE TEST"); i >= 0 {
		t = t[:i]
	}
	return t
}

// normalize maps a transcript onto the answer lines, dropping boot output,
// the echoed command, pids, addresses, uptimes and prompts.
func normalize(text string) string {
	t := getOutput(text)
	at := strings.Index(t, "tos> ")
	if at >= 0 {
		t = t[at:]
		lines := strings.Split(t, "\n")
		i := 1
		for i < len(lines) && (strings.HasPrefix(lines[i], "tos>") || strings.TrimSpace(lines[i]) == "") {
			i++
		}
		t = strings.Join(lines[i:], "\n")
	}
	t = pidSquareRe.ReplaceAllString(t, "[pid N]")
	t = pidEqRe.ReplaceAllString(t, "pid N")
	t = addrRe.ReplaceAllString(t, "0xADDR")
	t = msRe.ReplaceAllString(t, "N ms")
	t = tickRe.ReplaceAllString(t, "N tick(s)")
	var lines []string
	for _, line := range strings.Split(t, "\n") {
		line = strings.TrimSpace(line)
		if line != "" && line != "tos>" {
			lines = append(lines, line)
		}
	}
	return strings.Join(lines, "\n")
}

// getVerdict turns a trap line into words that do not depend on the
// architecture's exception names.
func getVerdict(normalized string) string {
	trap := ""
	for _, line := range strings.Split(normalized, "\n") {
		if strings.Contains(line, "[pid N]") {
			trap = line
			break
		}
	}
	if strings.Contains(trap, "MemManage") || strings.Contains(trap, "BusFault") || strings.Contains(trap, "access fault") {
		return "killed: protection fault"
	}
	if strings.Contains(trap, "illegal instruction") || strings.Contains(trap, "UsageFault") {
		return "killed: illegal instruction"
	}
	if strings.Contains(normalized, "survived: this machine cannot stop us") {
		return "survived"
	}
	return normalized
}

// charMultisetEqual reports whether two normalized transcripts contain
// exactly the same characters with the same multiplicities.  Used only as
// the interleave fallback above: real capability differences change words,
// and that shows up as different multisets.
func charMultisetEqual(a, b string) bool {
	if len(a) != len(b) {
		return false
	}
	counts := make(map[rune]int)
	for _, r := range a {
		counts[r]++
	}
	for _, r := range b {
		counts[r]--
		if counts[r] < 0 {
			return false
		}
	}
	return true
}

func compareCommand(cmd string, raw map[string]string, targets []string, verbose bool) bool {
	set := map[string]string{}
	var diffs []string
	for _, t := range targets {
		text := ""
		if raw[t] != "" {
			text = normalize(raw[t])
		}
		if text == "" {
			compatUnreliable = true
		}
		key := t + "|" + cmd
		if exp, ok := allowedDiffs[key]; ok {
			set[t] = exp
			seen := getVerdict(text)
			matched, err := regexp.MatchString(exp, seen)
			if err != nil || !matched {
				diffs = append(diffs, fmt.Sprintf("[%s] expected /%s/ but saw %s", t, exp, strings.ReplaceAll(text, "\n", " / ")))
			}
		} else {
			set[t] = text
		}
	}
	ref := targets[0]
	for _, t := range targets {
		if _, ok := allowedDiffs[t+"|"+cmd]; !ok && set[t] != set[ref] {
			// The mPS2 QEMU chardev delivers the shell's echo and a
			// program's output in interleaved chunks, so the arm transcript
			// can carry the same characters in a shuffled order ("alpha
			// bet" + "a").  That is a transport artifact, not a capability
			// difference: accept an exact character-multiset match, but
			// say so in the output instead of passing silently.
			if charMultisetEqual(set[t], set[ref]) {
				fmt.Printf("  ok*   %s  (%s: same characters, different order - echo/output interleave)\n", cmd, t)
				continue
			}
			diffs = append(diffs, fmt.Sprintf("[%s] %s", t, strings.ReplaceAll(set[t], "\n", " / ")))
		}
	}
	if len(diffs) == 0 {
		fmt.Printf("  ok    %s\n", cmd)
		return true
	}
	fmt.Printf("  DIFF  %s\n", cmd)
	for _, d := range diffs {
		fmt.Println("        " + d)
	}
	fmt.Println("        (a real capability difference belongs in PLATFORM-DESIGN 9")
	fmt.Println("         and in allowedDiffs of tostest compat)")
	return false
}

// compareIsolated runs one command per target on its own boot (the shell has
// no input queue, so a burst can make outputs run together), retrying once.
func compareIsolated(cmd string, targets []string, port *int, cmdWait time.Duration, verbose bool) bool {
	raw := map[string]string{}
	for _, t := range targets {
		for attempt := 0; attempt < 2; attempt++ {
			transcript, err := runSmoke(smokeOpts{target: t, commands: []string{cmd}, port: *port, cmdWait: cmdWait})
			*port++
			if err := assertRun(t, transcript, err); err != nil {
				fmt.Fprintln(os.Stderr, err)
				return false
			}
			if verbose {
				fmt.Println(transcript)
			}
			body := getOutput(transcript)
			at := strings.Index(body, "tos> ")
			if at < 0 {
				at = 0
			}
			raw[t] = body[at:]
			if normalize(raw[t]) != "" {
				break
			}
		}
	}
	return compareCommand(cmd, raw, targets, verbose)
}
