//go:build windows

package menu

import (
	"os"
	"syscall"
	"unsafe"
)

/* Windows consoles need ENABLE_VIRTUAL_TERMINAL_PROCESSING before they read
 * the ANSI escapes the menu draws with.  Windows Terminal sets it already;
 * conhost does not, and failing to set it would fill the screen with `[?25l`
 * instead of a menu. */

func enableVT() {
	kernel32 := syscall.NewLazyDLL("kernel32.dll")
	get := kernel32.NewProc("GetConsoleMode")
	set := kernel32.NewProc("SetConsoleMode")
	h := os.Stdout.Fd()
	var mode uint32

	if r, _, _ := get.Call(h, uintptr(unsafe.Pointer(&mode))); r == 0 {
		return
	}
	const enableVirtualTerminalProcessing = 0x0004
	set.Call(h, uintptr(mode|enableVirtualTerminalProcessing))
}
