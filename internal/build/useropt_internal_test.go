package build

import "testing"

func TestUserOptPrecedence(t *testing.T) {
	b := &Builder{Target: Target{UserOpt: "-Os"}}
	t.Setenv("TOS_USER_OPT", "")
	if got := b.userOptFlag(); got != "-Os" {
		t.Fatalf("target layer: got %q, want -Os", got)
	}
	b.UserOpt = "-O2"
	if got := b.userOptFlag(); got != "-O2" {
		t.Fatalf("config layer: got %q, want -O2", got)
	}
	t.Setenv("TOS_USER_OPT", "-Oz")
	if got := b.userOptFlag(); got != "-Oz" {
		t.Fatalf("env layer: got %q, want -Oz", got)
	}
	empty := &Builder{}
	t.Setenv("TOS_USER_OPT", "")
	if got := empty.userOptFlag(); got != "" {
		t.Fatalf("unset: got %q, want empty (inherit kernel)", got)
	}
}
