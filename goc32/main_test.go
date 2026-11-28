package main

import (
	"strings"
	"testing"
)

func TestNextStack(t *testing.T) {
	cases := map[int]int{
		100:   4096,
		4096:  4096,
		4097:  8192,
		8720:  16384, // 8208-byte frame + margin
		17000: 32768,
		32768: 32768,
	}
	for in, want := range cases {
		if got := nextStack(in); got != want {
			t.Fatalf("nextStack(%d) = %d, want %d", in, got, want)
		}
	}
}

func TestCheckStackFit(t *testing.T) {
	// 8 KiB local, no TOS_STACK: the error must suggest the size, not just
	// reject the array.
	err := checkStackFit("main", 8208, 0, 4096)
	if err == nil {
		t.Fatal("8 KiB frame accepted with the 4 KiB default")
	}
	for _, want := range []string{`"main"`, "8208", "add TOS_STACK(16384)"} {
		if !strings.Contains(err.Error(), want) {
			t.Fatalf("error %q does not mention %q", err, want)
		}
	}

	// Same frame, a 16 KiB declaration: fine.
	if err := checkStackFit("main", 8208, 16384, 4096); err != nil {
		t.Fatalf("8 KiB frame refused under TOS_STACK(16384): %v", err)
	}

	// Declaration too small: say "raise it", not "add".
	err = checkStackFit("main", 8208, 4096, 4096)
	if err == nil || !strings.Contains(err.Error(), "raise it to TOS_STACK(16384)") {
		t.Fatalf("small declaration not reported as a raise: %v", err)
	}

	// An ordinary program near the margin still passes.
	if err := checkStackFit("main", 3584, 0, 4096); err != nil {
		t.Fatalf("3.5 KiB frame refused with the 4 KiB default: %v", err)
	}

	// A deep chain is the kernel's business, not this check: only the
	// largest single frame is compared.
	if err := checkStackFit("main", 1024, 0, 4096); err != nil {
		t.Fatalf("1 KiB frame refused: %v", err)
	}
}
