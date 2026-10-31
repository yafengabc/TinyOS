package build

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// buildMklfsTool compiles the host-side littlefs image builder with zig.
func buildMklfsTool(t *testing.T, dir string) string {
	t.Helper()
	zig := findZig()
	if zig == "" {
		t.Skip("zig not found, skipping littlefs image test")
	}
	out := filepath.Join(dir, "mklfs.exe")
	root, err := FindRoot()
	if err != nil {
		t.Fatal(err)
	}
	cmd := exec.Command(zig, "cc", "-O2",
		"-I", filepath.Join(root, "tools"),
		"-I", filepath.Join(root, "third_party", "littlefs"),
		filepath.Join(root, "tools", "mklfs.c"),
		filepath.Join(root, "tools", "lz4.c"),
		filepath.Join(root, "third_party", "littlefs", "lfs.c"),
		filepath.Join(root, "third_party", "littlefs", "lfs_util.c"),
		"-o", out)
	if b, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("zig cc mklfs failed: %v\n%s", err, b)
	}
	return out
}

func TestLittlefsImageRoundTrip(t *testing.T) {
	dir := t.TempDir()
	mklfs := buildMklfsTool(t, dir)

	stage := filepath.Join(dir, "stage")
	for name, content := range map[string]string{
		"bin/hello":  "ELF-hello-payload",
		"readme.txt": "TinyOS littlefs test",
	} {
		dst := filepath.Join(stage, filepath.FromSlash(name))
		if err := os.MkdirAll(filepath.Dir(dst), 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(dst, []byte(content), 0o644); err != nil {
			t.Fatal(err)
		}
	}

	img := filepath.Join(dir, "lfs.img")
	args := []string{img, "307200", stage, "bin/hello", "readme.txt"}
	if b, err := exec.Command(mklfs, args...).CombinedOutput(); err != nil {
		t.Fatalf("mklfs create failed: %v\n%s", err, b)
	}

	st, err := os.Stat(img)
	if err != nil {
		t.Fatal(err)
	}
	if st.Size() != 307200 {
		t.Fatalf("image size = %d, want 307200 (75 * 4 KiB)", st.Size())
	}

	out, err := exec.Command(mklfs, "-v", img).CombinedOutput()
	if err != nil {
		t.Fatalf("mklfs verify failed: %v\n%s", err, out)
	}
	text := string(out)
	for _, want := range []string{"hello", "readme.txt"} {
		if !strings.Contains(text, want) {
			t.Errorf("verify output missing %q:\n%s", want, text)
		}
	}
}
