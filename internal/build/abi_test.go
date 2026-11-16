package build

import (
	"bufio"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"testing"
)

// The CSV is the ABI snapshot that toolchains outside this tree (notably
// goc32) consume.  The live source of truth is include/tos.h; this test keeps
// the snapshot from drifting silently.
func TestSyscallTableMatchesSnapshot(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	live := map[string]int{}
	src, err := os.ReadFile(filepath.Join(root, "include", "tos.h"))
	if err != nil {
		t.Fatal(err)
	}
	re := regexp.MustCompile(`(?m)^#define\s+(SYS_[A-Z0-9_]+)\s+(\d+)\b`)
	for _, m := range re.FindAllStringSubmatch(string(src), -1) {
		v, err := strconv.Atoi(m[2])
		if err != nil {
			t.Fatal(err)
		}
		live[m[1]] = v
	}

	f, err := os.Open(filepath.Join(root, "abi", "syscalls.csv"))
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	snap := map[string]int{}
	sc := bufio.NewScanner(f)
	first := true
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		if first {
			first = false
			if line == "name,value" {
				continue
			}
		}
		parts := strings.Split(line, ",")
		if len(parts) != 2 {
			t.Fatalf("bad CSV row %q", line)
		}
		v, err := strconv.Atoi(parts[1])
		if err != nil {
			t.Fatalf("%s: %v", line, err)
		}
		snap[parts[0]] = v
	}
	if err := sc.Err(); err != nil {
		t.Fatal(err)
	}
	for name, want := range live {
		if got, ok := snap[name]; !ok || got != want {
			t.Errorf("%s: snapshot=%d (present=%v), tos.h=%d", name, got, ok, want)
		}
	}
	for name := range snap {
		if _, ok := live[name]; !ok {
			t.Errorf("%s is in the snapshot but not include/tos.h", name)
		}
	}
}
