package build_test

import (
	"os"
	"path/filepath"
	"testing"

	"tinyos/internal/build"
)

func writeConfig(t *testing.T, text string) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "tos.conf")
	if err := os.WriteFile(p, []byte(text), 0o644); err != nil {
		t.Fatal(err)
	}
	return p
}

func TestConfigRejectsInvalidValues(t *testing.T) {
	cases := []string{
		"[build]\nheap = 1x\n",
		"[build]\nthreads = flase\n",
		"[build]\nheap = 9223372036854775807k\n",
		"[features]\npwm = maybe\n",
	}
	for _, text := range cases {
		if _, err := build.LoadConfig(writeConfig(t, text)); err == nil {
			t.Errorf("accepted invalid config:\n%s", text)
		}
	}
}

func TestConfigUserOpt(t *testing.T) {
	c, err := build.LoadConfig(writeConfig(t, "[build]\nopt = -Oz\nuser_opt = -O2\n"))
	if err != nil {
		t.Fatal(err)
	}
	if c.Opt != "-Oz" || c.UserOpt != "-O2" {
		t.Fatalf("opt=%q user_opt=%q", c.Opt, c.UserOpt)
	}
	for _, bad := range []string{"[build]\nuser_opt = -Ofast\n", "[build]\nopt = fast\n"} {
		if _, err := build.LoadConfig(writeConfig(t, bad)); err == nil {
			t.Errorf("accepted invalid opt: %s", bad)
		}
	}
}

func TestConfigAcceptsExplicitFalse(t *testing.T) {
	c, err := build.LoadConfig(writeConfig(t,
		"[build]\ntarget = rpi-pico\nheap = 16k\ndevnodes = 0\n"+
			"[features]\npwm = false\n"))
	if err != nil {
		t.Fatal(err)
	}
	if c.HeapBytes != 16*1024 {
		t.Errorf("heap = %d, want 16384", c.HeapBytes)
	}
	if c.DevNodes {
		t.Error("devnodes = 0 was not honoured")
	}
	if on, ok := c.Features["pwm"]; !ok || on {
		t.Errorf("pwm = false lost (present=%v on=%v)", ok, on)
	}
}
