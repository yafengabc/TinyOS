package main

// Headless mode for tests and scripting: tosput -cli <port> <local> [remote]

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// cliBuild reports whether this binary was linked as tosput-cli.exe: the
// same source builds both faces, and the name decides which one shows.
func cliBuild() bool {
	exe := strings.ToLower(filepath.Base(os.Args[0]))
	return strings.HasPrefix(exe, "tosput-cli")
}

func runCli() int {
	// Either "tosput-cli <port> <local>" or "tosput -cli <port> <local>":
	// the -cli token shifts everything by one.
	off := 1
	if len(os.Args) > 1 && os.Args[1] == "-cli" {
		off = 2
	}
	if len(os.Args) < off+2 {
		fmt.Fprintln(os.Stderr, "usage: tosput [-cli] <port> <local-file> [remote-path]")
		return 2
	}
	port, local := os.Args[off], os.Args[off+1]
	remote := ""
	if len(os.Args) > off+2 {
		remote = os.Args[off+2]
	}

	logf := func(format string, args ...interface{}) {
		fmt.Printf(format+"\n", args...)
	}
	var lastPct int
	progress := func(done, total int64) {
		if total == 0 {
			return
		}
		pct := int(done * 100 / total)
		if pct/10 != lastPct {
			lastPct = pct / 10
			fmt.Printf("  %3d%% (%d/%d)\n", pct, done, total)
		}
	}

	s, err := OpenSender(port, 115200, logf, progress, nil)
	if err != nil {
		fmt.Fprintln(os.Stderr, "open:", err)
		return 1
	}
	defer s.Close()

	t0 := time.Now()
	if err := s.SendFile(local, remote); err != nil {
		fmt.Fprintln(os.Stderr, "FAILED:", err)
		s.Abort()
		return 1
	}
	dt := time.Since(t0).Seconds()
	st, _ := os.Stat(local)
	if dt > 0 && st != nil {
		fmt.Printf("throughput: %.1f KiB/s\n", float64(st.Size())/1024.0/dt)
	}
	return 0
}
