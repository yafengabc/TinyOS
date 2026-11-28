package native32

import "testing"

// A local array is the frame shape a page fault cannot catch when it is
// bigger than the stack: `sub sp, #8192` crosses the guard hole and the first
// store lands in the writable image below it.  The back end records the
// frame so the command can compare it with the stack the program asks for.
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

func TestFrameHighWater(t *testing.T) {
	defer ResetFrames()

	m, err := ParseModule(bigFrameIR)
	if err != nil {
		t.Fatal(err)
	}
	ResetFrames()
	if _, err := EmitRV32(m); err != nil {
		t.Fatal(err)
	}
	if MaxFrame < 8192 || MaxFrameFunc != "main" {
		t.Fatalf("8 KiB alloca not recorded: %d in %q", MaxFrame, MaxFrameFunc)
	}

	m, err = ParseModule(smallFrameIR)
	if err != nil {
		t.Fatal(err)
	}
	ResetFrames()
	if _, err := EmitRV32(m); err != nil {
		t.Fatal(err)
	}
	if MaxFrame >= 512 {
		t.Fatalf("16-byte buffer recorded as a %d-byte frame", MaxFrame)
	}
}

func TestArmFrameHighWater(t *testing.T) {
	defer ResetFrames()

	m, err := ParseModule(bigFrameIR)
	if err != nil {
		t.Fatal(err)
	}
	ResetFrames()
	if _, err := EmitARM(m, "cortex-m0plus"); err != nil {
		t.Fatal(err)
	}
	if MaxFrame < 8192 || MaxFrameFunc != "main" {
		t.Fatalf("8 KiB alloca not recorded: %d in %q", MaxFrame, MaxFrameFunc)
	}
}
