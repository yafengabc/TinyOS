//go:build !windows

package main

import "os"

// The GUI is Windows-only (github.com/lxn/walk).  On other platforms the
// same source builds tosput, the headless CLI; there is no window to
// show.
func main() {
	os.Exit(runCli())
}
