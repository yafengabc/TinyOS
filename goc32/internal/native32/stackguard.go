package native32

import "fmt"

// StackGuard is the size of the target's stack-guard hole, in bytes.
//
// A function whose frame reaches this size can cross the hole with a single
// `sub sp, sp, #N`: the first store lands below the hole, in the writable
// image or heap, where no protection rule fires.  The page fault that
// normally turns a stack overflow into a kill never happens, so the frame is
// refused here instead, at compile time, naming the function.
//
// Zero disables the check.  The command sets it from -stack-guard; the
// default is 4 KiB, the hole on every board with a 4 KiB grain.
var StackGuard int

// FrameError is the first function whose frame reached StackGuard during the
// last emit, or "" when every frame fits.  EmitRV32 and EmitARM reset it.
var FrameError string

func checkFrame(name string, frame int) {
	if StackGuard <= 0 || frame < StackGuard || FrameError != "" {
		return
	}
	FrameError = fmt.Sprintf(
		"function %q has a %d-byte stack frame and the stack guard is %d bytes; "+
			"move large arrays to .bss or the heap, or split the function",
		name, frame, StackGuard)
}
