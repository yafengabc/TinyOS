package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

type benchConn interface {
	writeLine(string) error
	read(timeout time.Duration) ([]byte, bool)
	waitFor(needle []byte, timeout time.Duration) (bool, time.Duration)
	close()
}

func (s *session) writeLine(line string) error     { return s.sendLine(line, "\r\n") }
func (sc *socketConn) writeLine(line string) error { return sc.send(line) }

var (
	benchRe = regexp.MustCompile(`^bench: (\d+) B x (\d+) reps`)
	startRe = regexp.MustCompile(`^START (\S+)$`)
	endRe   = regexp.MustCompile(`^END (\S+) ticks=(-?\d+) sum=(-?\d+)$`)
	doneRe  = regexp.MustCompile(`(?m)^done$`)
)

// cmdBench implements `tostest bench`, the Go port of scripts/bench.py +
// scripts/esp-c3/benchtime.py: time the guest bench on RV32EMU-C or QEMU.
// The guest prints START/END marker lines; the host timestamps them because
// `uptime()` is an instruction-count tick on the emulator but wall time on
// QEMU, so guest ticks are reported next to host wall time.
func cmdBench(args []string) int {
	fs := flag.NewFlagSet("bench", flag.ExitOnError)
	qemuPort := fs.String("qemu", "", "time on a QEMU TCP serial port instead of the emulator")
	_ = fs.Parse(args)
	payload := ""
	if len(fs.Args()) > 0 {
		payload = strings.TrimSpace(fs.Args()[0])
	}

	var c benchConn
	label := "emu"
	if *qemuPort != "" {
		port, err := strconv.Atoi(*qemuPort)
		if err != nil {
			fmt.Fprintf(os.Stderr, "bad port %q\n", *qemuPort)
			return 2
		}
		conn, err := dialSerial(port, 20*time.Second)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		defer conn.close()
		ok, _ := conn.waitFor([]byte("tos> "), 60*time.Second)
		if !ok {
			fmt.Fprintln(os.Stderr, "no shell prompt from QEMU:\n"+conn.text())
			return 1
		}
		c = conn
		label = "qemu"
	} else {
		emu := findEmu()
		if !fileExists(emu) {
			fmt.Fprintf(os.Stderr, "RV32EMU-C not found: %s (run 'tostest emu build')\n", emu)
			return 1
		}
		elf := filepath.Join(repoRoot(), "build", "kernel-esp32c3.elf")
		rd := filepath.Join(repoRoot(), "build", "ramdisk-esp32c3.bin")
		if !fileExists(elf) {
			fmt.Fprintf(os.Stderr, "missing %s: run 'tos build -target esp32c3'\n", elf)
			return 1
		}
		s, err := startEmulator(emu, elf, rd)
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			return 1
		}
		defer s.close()
		ok, _ := s.waitFor([]byte("tos> "), 10*time.Second)
		if !ok {
			fmt.Fprintln(os.Stderr, "no shell prompt:\n"+s.tail(500))
			return 1
		}
		c = s
	}
	// Give the shell time to reach its read() so the command is not
	// type-ahead that the next tty_poll() could drop.
	time.Sleep(300 * time.Millisecond)

	line := "bench"
	if payload != "" {
		line = "bench " + payload
	}
	if err := c.writeLine(line); err != nil {
		fmt.Fprintln(os.Stderr, err)
		return 1
	}

	type stamp struct {
		t    time.Time
		line string
	}
	var stamps []stamp
	all := ""
	partial := ""
	deadline := time.Now().Add(900 * time.Second)
	for time.Now().Before(deadline) {
		b, more := c.read(200 * time.Millisecond)
		if b != nil {
			chunk := strings.ReplaceAll(string(b), "\r", "")
			all += chunk
			partial += chunk
			for {
				i := strings.IndexByte(partial, '\n')
				if i < 0 {
					break
				}
				line := strings.TrimSpace(partial[:i])
				partial = partial[i+1:]
				if line != "" {
					stamps = append(stamps, stamp{t: time.Now(), line: line})
				}
			}
			if doneRe.MatchString(all) {
				break
			}
		}
		if !more {
			break
		}
	}

	size, reps := 4096, 64
	for _, st := range stamps {
		if m := benchRe.FindStringSubmatch(st.line); m != nil {
			size, _ = strconv.Atoi(m[1])
			reps, _ = strconv.Atoi(m[2])
			break
		}
	}
	marks := map[string][2]stamp{}
	for _, st := range stamps {
		if m := startRe.FindStringSubmatch(st.line); m != nil {
			marks[m[1]] = [2]stamp{st, marks[m[1]][1]}
		}
		if m := endRe.FindStringSubmatch(st.line); m != nil {
			marks[m[1]] = [2]stamp{marks[m[1]][0], st}
		}
	}

	var t0 time.Time
	if len(stamps) > 0 {
		t0 = stamps[0].t
	}
	fmt.Println("---- " + label + " ----")
	for _, st := range stamps {
		fmt.Printf("%8.3f  %s\n", st.t.Sub(t0).Seconds(), st.line)
	}
	fmt.Println()
	fmt.Printf("%-9s %10s %13s %8s %10s %10s\n",
		"phase", "wall(ms)", "guest-ticks", "iters", "ns/iter", "ns/byte")
	for _, name := range []string{"a1-byte", "b-word", "a2-byte"} {
		m, ok := marks[name]
		if !ok || m[0].t.IsZero() || m[1].t.IsZero() {
			fmt.Printf("%-9s <not measured>\n", name)
			continue
		}
		iters := size * reps
		if strings.HasSuffix(name, "word") {
			iters = size / 4 * reps
		}
		wall := m[1].t.Sub(m[0].t).Seconds()
		var ticks int64
		if em := endRe.FindStringSubmatch(m[1].line); em != nil {
			ticks, _ = strconv.ParseInt(em[2], 10, 64)
		}
		fmt.Printf("%-9s %10.1f %13d %8d %10.1f %10.1f\n",
			name, wall*1000, ticks, iters,
			wall*1e9/float64(iters), wall*1e9/float64(size*reps))
	}
	fmt.Println()
	fmt.Println("guest ticks ~= emulated instructions/1000 under RunHot; QEMU's wall time is TCG, not an interpreter")
	return 0
}
