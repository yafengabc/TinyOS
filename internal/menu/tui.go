package menu

import (
	"fmt"
	"os"
	"sort"
	"strings"

	"golang.org/x/term"

	"tinyos/internal/build"
)

/* A small ncurses-style configuration editor.
 *
 * One page at a time: a list, a two-line help pane and a key bar.  Booleans
 * toggle with space, choices cycle with space/left/right, submenus open with
 * enter and leave with esc.  The state is the same State that menu.go writes
 * back to the INI file. */

const (
	kBool = iota
	kChoice
	kSub
	kBack
	kRadio
)

type row struct {
	label string
	help  string
	kind  int
	// note explains why a row is disabled ("required", "esp32c3 only", ...).
	note string
	// text is the right-hand value ("on", "esp32c3", ...).
	text func() string
	// act is called with 0 for space/enter and +/-1 for right/left.
	act func(dir int)
	// live reports whether the row can be edited.
	live func() bool
}

type page struct {
	title string
	rows  []row
}

type app struct {
	st    *State
	root  string
	progs []string

	stack []page
	cur   int
	off   int
	w, h  int

	dirty   bool
	status  string
	confirm bool
	quit    bool
}

func Run(st *State, root string) error {
	if !term.IsTerminal(int(os.Stdin.Fd())) || !term.IsTerminal(int(os.Stdout.Fd())) {
		return fmt.Errorf("menuconfig needs a terminal; use -print to dump the config instead")
	}
	enableVT()
	old, err := term.MakeRaw(int(os.Stdin.Fd()))
	if err != nil {
		return err
	}
	defer term.Restore(int(os.Stdin.Fd()), old)

	out := os.Stdout
	out.WriteString("\x1b[?1049h\x1b[?25l")       /* alternate screen, no cursor */
	defer out.WriteString("\x1b[?25h\x1b[?1049l") /* restore on every exit */

	a := &app{st: st, root: root, progs: Programs(root)}
	a.stack = []page{a.mainPage()}

	buf := make([]byte, 16)
	for !a.quit {
		a.draw()
		n, err := os.Stdin.Read(buf)
		if err != nil {
			return err
		}
		if n > 0 {
			a.handle(buf[:n])
		}
	}
	return nil
}

func (a *app) curPage() *page { return &a.stack[len(a.stack)-1] }

func (a *app) mainPage() page {
	return page{
		title: "TinyOS",
		rows: []row{
			{
				label: "Target", kind: kSub,
				text: func() string { return a.st.Target },
				act:  func(int) { a.push(a.targetPage()) },
				help: "Board/emulator the image is built for.  WiFi exists only on esp32c3.",
			},
			{
				label: "Kernel", kind: kSub,
				text: func() string {
					return fmt.Sprintf("heap %dK, stack %dK",
						a.st.heapActual()/1024, a.st.kstackActual()/1024)
				},
				act:  func(int) { a.push(a.kernelPage()) },
				help: "Heap arena, per-CPU kernel stack, process table size and optimisation level.",
			},
			{
				label: "Drivers", kind: kSub,
				text: func() string {
					n := len(a.st.DriverChoices())
					s := fmt.Sprintf("%d dirs", n)
					if a.st.WiFi() {
						s += " + wifi"
					}
					off := 0
					for range a.st.DriverExclude {
						off++
					}
					if off > 0 {
						s += fmt.Sprintf(", %d off", off)
					}
					return s
				},
				act:  func(int) { a.push(a.driverPage()) },
				help: "Optional driver directories for the selected target (display, USB, ...).",
			},
			{
				label: "IPv4 / DHCP / DNS / TCP", kind: kBool,
				text: func() string { return onOff(a.st.Net()) },
				act: func(d int) {
					if d == 0 {
						a.st.SetNet(!a.st.Net())
						a.dirty = true
					}
				},
				live: func() bool { return a.st.WiFi() },
				note: "needs the WiFi driver",
				help: "The built-in network stack on /dev/net: ARP, ICMP, DHCP client, DNS, TCP client and the http command.  Enable the WiFi driver first.",
			},
			{
				label: "Soft float: saturating (no NaN/Inf)", kind: kBool,
				text: func() string { return onOff(a.st.SoftMin()) },
				act: func(d int) {
					if d == 0 {
						a.st.SetFeature("softfloat-min", !a.st.SoftMin())
						a.dirty = true
					}
				},
				live: func() bool { return true },
				help: "Compile the IEEE float runtime with TOS_SOFTFLOAT_MIN: subnormals flush to zero, NaN/Inf saturate.  Saves about 580 bytes.",
			},
			{
				label: "User programs", kind: kSub,
				act:  func(int) { a.push(a.programPage()) },
				help: "Choose which user/*.c programs and user/<dir> interpreters are packed into the littlefs image.",
			},
		},
	}
}

func (a *app) targetPage() page {
	names := targetNames()
	rows := make([]row, 0, len(names)+1)
	for _, name := range names {
		name := name
		t := build.Targets[name]
		desc := t.Arch + " / " + t.Board
		if t.CPUs > 1 {
			desc += fmt.Sprintf(" / %d cores", t.CPUs)
		}
		rows = append(rows, row{
			label: name, kind: kRadio,
			text: func() string {
				if a.st.Target == name {
					return "(*)"
				}
				return "( )"
			},
			act: func(int) {
				a.st.SetTarget(name)
				a.dirty = true
				a.pop()
			},
			help: desc + " -- " + targetBlurb(name),
		})
	}
	rows = append(rows, row{
		label: "< Back", kind: kBack,
		act:  func(int) { a.pop() },
		help: "Return without changing the target.",
	})
	return page{title: "Target", rows: rows}
}

func targetBlurb(name string) string {
	switch name {
	case "x86":
		return "the PC image, boots in QEMU from a disk with the littlefs image"
	case "rv32":
		return "qemu-virt RISC-V, the reference for the MCU ports"
	case "rv32smp", "rp2350smp", "rp2350rvsmp":
		return "both cores"
	case "esp32c3":
		return "the only WiFi target"
	case "rp2040":
		return "Cortex-M0+, no FPU, no hardware divide"
	case "rp2350":
		return "Cortex-M33"
	case "rp2350rv":
		return "RP2350 Hazard3 (RISC-V)"
	case "arm":
		return "qemu mps2-an385 Cortex-M3"
	}
	return ""
}

func (a *app) kernelPage() page {
	heaps := []int{0, 8 << 10, 12 << 10, 16 << 10, 24 << 10, 32 << 10,
		48 << 10, 64 << 10, 80 << 10, 96 << 10, 128 << 10, 192 << 10, 256 << 10}
	stacks := []int{0, 8 << 10, 16 << 10, 32 << 10}
	procs := []int{0, 4, 6, 8, 12, 16}
	opts := []string{"", "-O2", "-Oz", "-Os", "-Og"}
	hists := []int{0, 256, 512, 1024}

	return page{
		title: "Kernel",
		rows: []row{
			{
				label: "Heap arena", kind: kChoice,
				text: func() string {
					if a.st.HeapBytes > 0 {
						return fmt.Sprintf("%d KiB", a.st.HeapBytes/1024)
					}
					return fmt.Sprintf("%d KiB (target)", a.st.HeapDefault()/1024)
				},
				act:  cycleInt(heaps, func() int { return a.st.HeapBytes }, func(v int) { a.st.HeapBytes = v; a.dirty = true }),
				live: func() bool { return true },
				help: "Kernel malloc arena (TOS_HEAP_BYTES).  littlefs and exec work out of it; 8 KiB is the measured floor on the C3.  Growing it moves the kernel .bss end, and the task area (which starts at the link-time __app_base) shrinks or grows to match.  The link fails with 'kernel leaves no room for a task window' if none would fit.",
			},
			{
				label: "Kernel stack", kind: kChoice,
				text: func() string {
					if a.st.KStack > 0 {
						return fmt.Sprintf("%d KiB", a.st.KStack/1024)
					}
					return fmt.Sprintf("%d KiB (arch)", a.st.KStackDefault()/1024)
				},
				act:  cycleInt(stacks, func() int { return a.st.KStack }, func(v int) { a.st.KStack = v; a.dirty = true }),
				live: func() bool { return true },
				help: "Per-CPU trap/syscall stack (TOS_KSTACK_BYTES).  The linker pool and kernel/cpu.h read the same value, and cpu_init() panics if they disagree.",
			},
			{
				label: "Max processes", kind: kChoice,
				text: func() string {
					if a.st.MaxProcs > 0 {
						return fmt.Sprintf("%d", a.st.MaxProcs)
					}
					return fmt.Sprintf("%d (default)", a.st.ProcsDefault())
				},
				act:  cycleInt(procs, func() int { return a.st.MaxProcs }, func(v int) { a.st.MaxProcs = v; a.dirty = true }),
				live: func() bool { return true },
				help: "Process table slots (TOS_MAX_PROCS).  sizeof(proc_t) is 856 B on the C3 (measured: fds[12] 432, saved trap frame 144, cwd 64), so 8 slots cost 6.8 KiB of .bss and 4 cost 3.4 KiB; 4 is the floor for a shell plus one child, 8 leaves room for a few background tasks.",
			},
			{
				label: "Optimisation", kind: kChoice,
				text: func() string {
					if a.st.Opt != "" {
						return a.st.Opt
					}
					return "-O2 (default)"
				},
				act:  cycleStr(opts, func() string { return a.st.Opt }, func(v string) { a.st.Opt = v; a.dirty = true }),
				live: func() bool { return true },
				help: "Compiler optimisation (opt = ... in [build]).  TOS_OPT in the environment still wins; -Oz is smaller, -O2 is faster under QEMU.",
			},
			{
				label: "Threads (cooperative)", kind: kBool,
				text: func() string { return onOff(a.st.Threads) },
				act: func(int) {
					a.st.Threads = !a.st.Threads
					a.dirty = true
				},
				live: func() bool { return true },
				help: "Build the user-space thread library (threads = 1 in [build]).  thread_create/join, a stack per thread from the program's heap, and mutex_lock; switches happen only at yield/thread_read, so a plain read() still parks the whole process.  Adds ~1 KiB to each program that uses it and nothing to the kernel.",
			},
			{
				label: "Command history", kind: kChoice,
				text: func() string {
					if a.st.HistBytes > 0 {
						return fmt.Sprintf("%d B", a.st.HistBytes)
					}
					return "off"
				},
				act:  cycleInt(hists, func() int { return a.st.HistBytes }, func(v int) { a.st.HistBytes = v; a.dirty = true }),
				live: func() bool { return true },
				help: "Line-discipline history pool (TOS_HIST_BYTES).  Off removes the arrow-key recall and the `history` builtin's data, saving up to 1 KiB of .bss and a little text; the rest of line editing stays.",
			},
			{
				label: "Diagnostic /proc", kind: kBool,
				text: func() string {
					if a.st.ProcLite {
						return "ps + uptime only"
					}
					return "all entries"
				},
				act: func(int) {
					a.st.ProcLite = !a.st.ProcLite
					a.dirty = true
				},
				live: func() bool { return true },
				help: "Drops /proc/{mem,dev,lfs,stack,cpuinfo} (proc_lite = 1) and keeps /proc/{ps,uptime}, which msh's `ps` needs.  Saves ~1.5 KiB of text; the matching meminfo()/dfinfo() syscalls still work for free and df.",
			},
			{
				label: "Device layer (/dev, GPIO/ADC)", kind: kBool,
				text: func() string { return onOff(a.st.DevNodes) },
				act: func(int) {
					a.st.DevNodes = !a.st.DevNodes
					a.dirty = true
				},
				live: func() bool { return true },
				help: "Compiles out kernel/hwdev.c and kernel/periph.c: /dev/{led,key,gpio,adc,lcd}, the GPIO/ADC syscalls and the board device table.  Saves ~5.5 KiB of text and ~2 KiB of .bss on a headless board; program calls fail with -1.  The panel mirror follows this switch.",
			},
			{
				label: "Panel text mirror", kind: kBool,
				text: func() string { return onOff(a.st.Panel) },
				act: func(int) {
					a.st.Panel = !a.st.Panel
					a.dirty = true
				},
				live: func() bool { return true },
				help: "Compiles out kernel/screen.c, the console-on-panel mirror and its 20x48 shadow grid (panel = 0).  Saves ~2.2 KiB of text and 960 B of .bss on boards with no LCD.",
			},
			{
				label: "Read-only filesystem", kind: kBool,
				text: func() string { return onOff(a.st.FSReadOnly) },
				act: func(int) {
					a.st.FSReadOnly = !a.st.FSReadOnly
					a.dirty = true
				},
				live: func() bool { return true },
				help: "Builds littlefs with LFS_READONLY and stubs out create/write/unlink (fs_ro = 1).  Several KiB of text for an appliance image; `edit` and shell redirection then fail with -1 instead of writing.",
			},
			{
				label: "Mailbox (4 x 256 B)", kind: kBool,
				text: func() string { return onOff(a.st.Mailbox) },
				act: func(int) {
					a.st.Mailbox = !a.st.Mailbox
					a.dirty = true
				},
				live: func() bool { return true },
				help: "Global message pool: msg_send copies into one of four 256-byte slots (non-blocking), msg_recv copies out and blocks by sleeping 1 ms.  One KiB of .bss and a few hundred bytes of text, no per-task buffers and no PMP/MPU changes; off removes the pool entirely.",
			},
			{
				label: "< Back", kind: kBack,
				act:  func(int) { a.pop() },
				help: "Return to the main menu.",
			},
		},
	}
}

func (a *app) driverPage() page {
	var rows []row
	/* WiFi is a driver too: the directory is only half of it, the other half
	 * is the Espressif blob set and its ROM linker script, which the [wifi]
	 * section names.  Keeping the switch here makes that visible. */
	rows = append(rows, row{
		label: "WiFi (esp32c3 blobs + WPA2)", kind: kBool,
		text: func() string { return onOff(a.st.WiFi()) },
		act: func(int) {
			a.st.SetWiFi(!a.st.WiFi())
			a.dirty = true
		},
		live: func() bool { return a.st.WiFiCapable() },
		note: "esp32c3 only",
		help: "Adds drivers/wifi/esp32c3 and links the Espressif net80211/pp/phy/coexist blobs with the ROM linker script; the [wifi] paths are filled in on save.  The IPv4 stack has its own switch.",
	})

	/* RP2040 peripheral drivers live in board/rp2040 (not in a driver
	 * directory), so they need feature switches rather than DriverExclude.
	 * Each one is a real #ifdef around the driver; turning it off removes
	 * the code and the matching user command. */
	for _, pd := range []struct {
		name, label, help string
	}{
		{"pwm", "PWM output",
			"Board PWM driver (RP2040 slices / C3 LEDC), SYS_PWM and the pwm command.  Off removes the driver code and the command."},
		{"uart1", "UART1 (extra serial port)",
			"Second serial port with a non-blocking read, SYS_UART_* and the uart command.  UART0 remains the console."},
		{"spi", "SPI master",
			"SPI master driver, software chip select, SYS_SPI and the spi command.  Off also drops the spi user program."},
		{"i2c", "I2C master",
			"I2C master driver, SYS_I2C and the i2c command."},
		{"pio", "PIO (2 blocks / 8 state machines)",
			"RP2040 PIO loader, SYS_PIO and the user-space assembler (pio command).  Off removes the driver and the command."},
		{"dma", "DMA memory-to-memory copy",
			"RP2040 DMA memcpy, SYS_DMA_MEMCPY and the dma command."},
		{"kthreads", "Kernel threads (experimental)",
			"Threads scheduled by the kernel: a thread is a table slot sharing the caller's window and heap, preempted by the tick, with per-thread blocking.  Adds SYS_THREAD_CREATE/EXIT/JOIN and a 16 B larger proc_t; threadbench evaluates it."},
	} {
		pd := pd
		rows = append(rows, row{
			label: pd.label, kind: kBool,
			text: func() string { return onOff(a.st.Feature(pd.name)) },
			act: func(int) {
				a.st.SetFeature(pd.name, !a.st.Feature(pd.name))
				a.dirty = true
			},
			live: func() bool { return a.st.DefaultFeature(pd.name) },
			note: "rp2040 only",
			help: pd.help,
		})
	}

	dirs := a.st.DriverChoices()
	for _, d := range dirs {
		d := d
		required := DriverRequired(d)
		rows = append(rows, row{
			label: d, kind: kBool,
			text: func() string {
				if a.st.DriverExclude[d] {
					return "off"
				}
				return "on"
			},
			act: func(int) {
				if required {
					return
				}
				if a.st.DriverExclude[d] {
					delete(a.st.DriverExclude, d)
				} else {
					a.st.DriverExclude[d] = true
				}
				a.dirty = true
			},
			live: func() bool { return !required },
			note: "required",
			help: "Compile the sources in this directory.  Disabling an optional driver saves its code (and sometimes .bss), but programs that use it stop working.",
		})
	}
	rows = append(rows, row{
		label: "< Back", kind: kBack,
		act:  func(int) { a.pop() },
		help: "Return to the main menu.",
	})
	return page{title: "Drivers", rows: rows}
}

func cycleInt(vals []int, get func() int, set func(int)) func(int) {
	return func(dir int) {
		if dir == 0 {
			dir = 1
		}
		cur := 0
		for i, v := range vals {
			if v == get() {
				cur = i
				break
			}
		}
		cur = (cur + dir + len(vals)) % len(vals)
		set(vals[cur])
	}
}

func cycleStr(vals []string, get func() string, set func(string)) func(int) {
	return func(dir int) {
		if dir == 0 {
			dir = 1
		}
		cur := 0
		for i, v := range vals {
			if v == get() {
				cur = i
				break
			}
		}
		cur = (cur + dir + len(vals)) % len(vals)
		set(vals[cur])
	}
}

func (a *app) programPage() page {
	rows := make([]row, 0, len(a.progs)+1)
	for _, name := range a.progs {
		if name == "threadtest" && !a.st.Threads {
			continue /* only meaningful when the library is built */
		}
		if name == "mailtest" && !a.st.Mailbox {
			continue
		}
		name := name
		rows = append(rows, row{
			label: name, kind: kBool,
			text: func() string {
				if a.st.Exclude[name] {
					return "off"
				}
				return "on"
			},
			act: func(d int) {
				if d != 0 {
					return
				}
				if a.st.Exclude[name] {
					delete(a.st.Exclude, name)
				} else {
					a.st.Exclude[name] = true
				}
				a.dirty = true
			},
			live: func() bool { return true },
			help: "Included in the image: " + name + ".  Toggling only changes [user] exclude.",
		})
	}
	rows = append(rows, row{
		label: "< Back", kind: kBack,
		act:  func(int) { a.pop() },
		help: "Return to the main menu.",
	})
	return page{title: "User programs", rows: rows}
}

func (a *app) push(p page) {
	a.stack = append(a.stack, p)
	a.cur, a.off = 0, 0
	a.status = ""
}

func (a *app) pop() {
	if len(a.stack) > 1 {
		a.stack = a.stack[:len(a.stack)-1]
		a.cur, a.off = 0, 0
		a.status = ""
	}
}

func (a *app) move(d int) {
	n := len(a.curPage().rows)
	if n == 0 {
		return
	}
	a.cur = (a.cur + d + n) % n
	a.status = ""
}

func (a *app) activate(dir int) {
	p := a.curPage()
	if a.cur >= len(p.rows) {
		return
	}
	r := p.rows[a.cur]
	if r.live != nil && !r.live() {
		a.status = "disabled here: " + r.label
		return
	}
	if r.kind == kBool && dir != 0 {
		return
	}
	if r.act != nil {
		r.act(dir)
	}
	a.status = ""
}

func (a *app) handle(b []byte) {
	if b[0] == 3 { /* ctrl-c */
		a.quit = true
		return
	}
	if b[0] == 'q' {
		if a.dirty && !a.confirm {
			a.confirm = true
			a.status = "unsaved changes: q again discards, s saves"
		} else {
			a.quit = true
		}
		return
	}
	a.confirm = false

	if b[0] == 27 {
		if len(b) >= 3 && b[1] == '[' {
			switch b[2] {
			case 'A':
				a.move(-1)
			case 'B':
				a.move(1)
			case 'C':
				a.activate(+1)
			case 'D':
				a.activate(-1)
			}
			return
		}
		a.pop()
		return
	}

	switch b[0] {
	case 'k':
		a.move(-1)
	case 'j':
		a.move(1)
	case ' ', '\r', '\n':
		a.activate(0)
	case 's':
		a.save()
	}
}

func (a *app) save() {
	if err := a.st.Save(); err != nil {
		a.status = "save failed: " + err.Error()
		return
	}
	a.dirty = false
	a.status = "saved " + a.st.Path
}

func (a *app) draw() {
	w, h, err := term.GetSize(int(os.Stdout.Fd()))
	if err != nil || w <= 0 || h <= 0 {
		w, h = 80, 24
	}
	lines := a.render(w, h)
	var b strings.Builder
	b.WriteString("\x1b[H")
	for i, line := range lines {
		b.WriteString(line)
		if i < len(lines)-1 {
			b.WriteString("\r\n")
		}
	}
	os.Stdout.WriteString(b.String())
}

/* render builds exactly h lines of width w.  Split from draw so the layout can
 * be exercised without a terminal. */
func (a *app) render(w, h int) []string {
	if w < 20 {
		w = 20
	}
	if h < 8 {
		h = 8
	}
	a.w, a.h = w, h

	lines := make([]string, 0, h)
	p := a.curPage()
	title := " TinyOS configuration  " + a.st.Path
	if len(a.stack) > 1 {
		title += "  /  " + p.title
	}
	if a.dirty {
		title += "  *"
	}
	lines = append(lines, style(pad(title, w), "\x1b[1m"))

	listH := h - 6
	if listH < 2 {
		listH = 2
	}
	if a.cur < a.off {
		a.off = a.cur
	}
	if a.cur >= a.off+listH {
		a.off = a.cur - listH + 1
	}
	if a.off < 0 {
		a.off = 0
	}
	lines = append(lines, pad("", w))
	for i := 0; i < listH; i++ {
		idx := a.off + i
		if idx >= len(p.rows) {
			lines = append(lines, pad("", w))
			continue
		}
		r := p.rows[idx]
		enabled := r.live == nil || r.live()
		txt := a.rowText(r, enabled)
		if idx == a.cur {
			txt = "\x1b[7m" + pad(txt, w) + "\x1b[0m"
		} else if !enabled {
			txt = "\x1b[2m" + pad(txt, w) + "\x1b[0m"
		} else {
			txt = pad(txt, w)
		}
		lines = append(lines, txt)
	}

	for len(lines) < h-4 {
		lines = append(lines, pad("", w))
	}
	lines = append(lines, strings.Repeat("-", w))

	help := ""
	if a.cur < len(p.rows) {
		help = p.rows[a.cur].help
	}
	hl := wrap(help, w)
	for i := 0; i < 2; i++ {
		s := ""
		if i < len(hl) {
			s = hl[i]
		}
		lines = append(lines, style(pad(s, w), "\x1b[2m"))
	}

	if a.status != "" {
		lines = append(lines, style(pad(a.status, w), "\x1b[1m"))
	} else {
		lines = append(lines, pad("arrows move   space toggle   enter open   esc back   s save   q quit", w))
	}
	return lines[:h]
}

func (a *app) rowText(r row, enabled bool) string {
	var left string
	switch r.kind {
	case kBool:
		box := "[ ]"
		if r.text != nil && r.text() == "on" {
			box = "[x]"
		}
		left = box + " " + r.label
	case kRadio:
		mark := "( )"
		if r.text != nil {
			mark = r.text()
		}
		left = "  " + mark + " " + r.label
	case kBack:
		return r.label
	default:
		left = "  " + r.label
		if r.text != nil {
			left += "  " + r.text()
		}
	}
	if r.kind == kSub {
		left += "  >"
	}
	if !enabled {
		if r.note != "" {
			left += "  (" + r.note + ")"
		} else {
			left += "  (unavailable)"
		}
	}
	return left
}

func targetNames() []string {
	names := make([]string, 0, len(build.Targets))
	for n := range build.Targets {
		names = append(names, n)
	}
	sort.Strings(names)
	return names
}

func onOff(b bool) string {
	if b {
		return "on"
	}
	return "off"
}

func style(s, code string) string { return code + s + "\x1b[0m" }

func pad(s string, w int) string {
	n := len(s)
	if n > w {
		return s[:w]
	}
	return s + strings.Repeat(" ", w-n)
}

func wrap(s string, w int) []string {
	words := strings.Fields(s)
	var out []string
	line := ""
	for _, word := range words {
		if line == "" {
			line = word
			continue
		}
		if len(line)+1+len(word) > w {
			out = append(out, line)
			line = word
			continue
		}
		line += " " + word
	}
	if line != "" {
		out = append(out, line)
	}
	return out
}
