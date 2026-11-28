package native32

import (
	"strings"
	"testing"
)

// A local array larger than the guard hole is the one frame shape a page
// fault cannot catch: `sub sp, #8192` crosses the hole and the first store
// lands in the writable image below it.  The back end must refuse it.
const bigFrameIR = `
define i32 @main(i32 %p0, ptr %p1) {
entry:
  %big = alloca [8192 x i8]
  %p = getelementptr [8192 x i8], ptr %big, i32 0, i32 0
  store i8 1, ptr %p
  ret i32 0
}
`

const smallFrameIR = `
define i32 @main(i32 %p0, ptr %p1) {
entry:
  %buf = alloca [16 x i8]
  %p = getelementptr [16 x i8], ptr %buf, i32 0, i32 0
  store i8 1, ptr %p
  ret i32 0
}
`

func TestStackGuardRefusesLargeFrame(t *testing.T) {
	defer func() { StackGuard = 0; FrameError = "" }()

	m, err := ParseModule(bigFrameIR)
	if err != nil {
		t.Fatal(err)
	}
	StackGuard = 4096
	if _, err := EmitRV32(m); err != nil {
		t.Fatal(err)
	}
	if FrameError == "" {
		t.Fatal("an 8 KiB frame was accepted with a 4 KiB guard")
	}
	if !strings.Contains(FrameError, "main") || !strings.Contains(FrameError, "stack frame") {
		t.Fatalf("frame error does not name the function and the size: %q", FrameError)
	}
}

func TestStackGuardAcceptsSmallFrame(t *testing.T) {
	defer func() { StackGuard = 0; FrameError = "" }()

	m, err := ParseModule(smallFrameIR)
	if err != nil {
		t.Fatal(err)
	}
	StackGuard = 4096
	if _, err := EmitRV32(m); err != nil {
		t.Fatal(err)
	}
	if FrameError != "" {
		t.Fatalf("a 16-byte buffer was refused: %q", FrameError)
	}
}

func TestStackGuardZeroDisables(t *testing.T) {
	defer func() { StackGuard = 0; FrameError = "" }()

	m, err := ParseModule(bigFrameIR)
	if err != nil {
		t.Fatal(err)
	}
	StackGuard = 0
	if _, err := EmitRV32(m); err != nil {
		t.Fatal(err)
	}
	if FrameError != "" {
		t.Fatalf("check ran with the limit disabled: %q", FrameError)
	}
}
