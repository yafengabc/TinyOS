package build

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

// goc32 keeps its own copy of the TinyOS syscall table (ARCHITECTURE-NEXT
// 3.1): goc32 must build without this checkout, so the table it targets is a
// committed source file there, with the TinyOS commit and the CSV's sha256 in
// a comment header.
//
// Two files, two owners: abi/syscalls.csv is the truth, goc32's snapshot is a
// copy.  This test is where they meet - it fails when someone adds a syscall
// and forgets scripts/syscalls-snapshot.py, which is the only way that gate
// can actually hold.
//
// The goc32 side has its own test (goc32/abi) for the half that works without
// this tree: the header must match the body's hash.
func TestGoc32SyscallSnapshotMatchesTable(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}

	csvPath := filepath.Join(root, "abi", "syscalls.csv")
	snapPath := filepath.Join(root, "goc32", "abi", "syscalls.snapshot.csv")
	csv, err := os.ReadFile(csvPath)
	if err != nil {
		t.Fatal(err)
	}
	raw, err := os.ReadFile(snapPath)
	if err != nil {
		t.Skipf("goc32 submodule not checked out: %v", err)
	}

	header, body := splitSnapshotHeader(string(raw))

	if body != string(csv) {
		t.Errorf("goc32/abi/syscalls.snapshot.csv no longer matches abi/syscalls.csv;\n"+
			"regenerate with: python scripts/syscalls-snapshot.py\n"+
			"  snapshot has %d lines, abi/syscalls.csv has %d",
			strings.Count(body, "\n"), strings.Count(string(csv), "\n"))
	}

	sum := sha256.Sum256(csv)
	want := headerField(header, "tinyos_csv_sha256")
	if got := hex.EncodeToString(sum[:]); got != want {
		t.Errorf("snapshot records tinyos_csv_sha256 %q, but abi/syscalls.csv hashes to %q;\n"+
			"regenerate with: python scripts/syscalls-snapshot.py", want, got)
	}

	// tinyos_commit is provenance (the tree the copy was taken from), not a
	// synchronisation guarantee: the snapshot is committed after that commit.
	// Requiring the object to exist still catches a made-up or truncated sha,
	// but CI checks out a shallow clone where older commits are simply not
	// there - that is a property of the checkout, not a stale snapshot.
	commit := headerField(header, "tinyos_commit")
	if !regexp.MustCompile(`^[0-9a-f]{40}$`).MatchString(commit) {
		t.Fatalf("tinyos_commit is not a full sha1: %q", commit)
	}
	if _, err := exec.LookPath("git"); err == nil && !shallowRepo(root) {
		cmd := exec.Command("git", "-C", root, "cat-file", "-t", commit)
		out, err := cmd.CombinedOutput()
		if err != nil || strings.TrimSpace(string(out)) != "commit" {
			t.Errorf("tinyos_commit %s is not a commit in this repository (%s):\n%s",
				commit, strings.TrimSpace(string(out)), out)
		}
	}
}

// splitSnapshotHeader separates the "# ..." provenance header from the CSV
// body.  The body is the part that has to stay byte-identical to the real
// table, so the split must not normalise anything.
func splitSnapshotHeader(text string) (header, body string) {
	var head, rest strings.Builder
	inBody := false
	for _, line := range strings.SplitAfter(text, "\n") {
		if !inBody && strings.HasPrefix(line, "#") {
			head.WriteString(line)
			continue
		}
		inBody = true
		rest.WriteString(line)
	}
	return head.String(), rest.String()
}

func headerField(header, key string) string {
	re := regexp.MustCompile(`(?m)^#\s+` + regexp.QuoteMeta(key) + `:\s*(\S+)\s*$`)
	m := re.FindStringSubmatch(header)
	if m == nil {
		return ""
	}
	return m[1]
}

// shallowRepo reports whether the checkout only has recent history, which is
// what actions/checkout does by default.
func shallowRepo(root string) bool {
	out, err := exec.Command("git", "-C", root, "rev-parse",
		"--is-shallow-repository").Output()
	return err == nil && strings.TrimSpace(string(out)) == "true"
}
