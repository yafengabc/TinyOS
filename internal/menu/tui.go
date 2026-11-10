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
)

type row struct {
	label string
	help  string
	kind  int
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
	names := targetNames()
	pick := func(dir int) func(int) {
		return func(d int) {
			if d == 0 {
				d = dir
			}
			i := 0
			for i < len(names) && names[i] != a.st.Target {
				i++
			}
			if i == len(names) {
				i = 0
			}
			i = (i + d + len(names)) % len(names)
			a.st.SetTarget(names[i])
			a.dirty = true
		}
	}
	return page{
		title: "TinyOS",
		rows: []row{
			{
				label: "Target", kind: kChoice,
				text: func() string { return a.st.Target },
				act:  pick(+1), live: func() bool { return true },
				help: "Board/emulator the image is built for.  WiFi exists only on esp32c3.",
			},
			{
				label: "WiFi support", kind: kBool,
				text: func() string { return onOff(a.st.WiFi()) },
				act: func(d int) {
					if d == 0 {
						a.st.SetWiFi(!a.st.WiFi())
						a.dirty = true
					}
				},
				live: func() bool { return a.st.WiFiCapable() },
				help: "Link the Espressif WiFi/PHY blobs plus the WPA2 supplicant.  Enabling it also adds the driver directory and the [wifi] paths on save.",
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
				help: "The built-in network stack on /dev/net: ARP, ICMP, DHCP client, DNS, TCP client and the http command.",
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

func (a *app) programPage() page {
	rows := make([]row, 0, len(a.progs)+1)
	for _, name := range a.progs {
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
		r := p.rows[a.cur]
		help = r.help
		if r.live != nil && !r.live() && r.kind == kBool && r.label == "WiFi support" {
			help = "WiFi is only available on esp32c3; change the target first."
		}
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
	case kBack:
		return r.label
	default:
		left = "  " + r.label
	}
	if r.kind == kChoice && r.text != nil {
		left = "  " + r.label + "  " + r.text()
	}
	if r.kind == kSub {
		left += "  >"
	}
	if !enabled {
		left += "  (unavailable)"
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
