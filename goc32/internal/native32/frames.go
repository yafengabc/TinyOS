package native32

// MaxFrame is the largest stack frame any function in the last program used,
// and MaxFrameFunc names it.  ResetFrames starts a new program; the emitters
// only raise the number.
//
// The command compares it with the stack the program asks for (TOS_STACK) or
// the board default.  A frame that does not fit is refused there, with the
// size to declare: a frame bigger than the stack crosses the guard hole in
// one `sub sp, sp, #N`, and the first store then lands in the writable image
// below it, where no page fault stops it.
var (
	MaxFrame     int
	MaxFrameFunc string
)

// ResetFrames clears the high-water mark before a program is compiled.
func ResetFrames() {
	MaxFrame = 0
	MaxFrameFunc = ""
}

func noteFrame(name string, frame int) {
	if frame > MaxFrame {
		MaxFrame = frame
		MaxFrameFunc = name
	}
}
