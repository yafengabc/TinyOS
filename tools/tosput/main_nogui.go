//go:build !windows

package main

import "os"

// The GUI is Windows-only (github.com/lxn/walk).  On other platforms the
// same source builds the headless tosput-cli; there is no window to show.
func main() {
	os.Exit(runCli())
}
