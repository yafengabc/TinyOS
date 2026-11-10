package menu

import (
	"path/filepath"
	"strings"
	"testing"

	"tinyos/internal/build"
)

func TestRenderRoundTrip(t *testing.T) {
	path := filepath.Join(t.TempDir(), "menu.conf")
	st := Load(nil, path)
	st.SetTarget("esp32c3")
	st.SetWiFi(true)
	st.SetNet(true)
	st.SetFeature("softfloat-min", true)
	st.Exclude["ping"] = true
	st.Exclude["http"] = true
	st.LDExtra = []string{"-Wl,--gc-sections"}
	st.HeapBytes = 48 << 10
	st.MaxProcs = 8
	st.KStack = 8 << 10
	st.Opt = "-Oz"
	st.Threads = true
	st.HistBytes = 256
	st.ProcLite = true
	st.DevNodes = false
	st.Panel = false
	st.FSReadOnly = true
	st.DriverExclude["drivers/display/st7789/rp2350"] = true

	if err := st.Save(); err != nil {
		t.Fatal(err)
	}
	cfg, err := build.LoadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	got := Load(cfg, path)

	if got.Target != "esp32c3" {
		t.Fatalf("target = %q", got.Target)
	}
	if !got.WiFi() || !got.Net() || !got.SoftMin() {
		t.Fatalf("features lost: %+v", got.Features)
	}
	if !got.Exclude["ping"] || !got.Exclude["http"] {
		t.Fatalf("excludes lost: %+v", got.Exclude)
	}
	if got.HeapBytes != 48<<10 || got.MaxProcs != 8 || got.KStack != 8<<10 ||
		got.Opt != "-Oz" {
		t.Fatalf("kernel knobs lost: heap=%d procs=%d stack=%d opt=%q",
			got.HeapBytes, got.MaxProcs, got.KStack, got.Opt)
	}
	if !got.Threads {
		t.Fatal("threads flag lost")
	}
	if got.HistBytes != 256 || !got.ProcLite || got.DevNodes || got.Panel ||
		!got.FSReadOnly {
		t.Fatalf("trim knobs lost: hist=%d proclite=%v dev=%v panel=%v ro=%v",
			got.HistBytes, got.ProcLite, got.DevNodes, got.Panel, got.FSReadOnly)
	}
	if !got.DriverExclude["drivers/display/st7789/rp2350"] {
		t.Fatalf("driver exclude lost: %+v", got.DriverExclude)
	}
	drivers := got.EffectiveDrivers()
	if len(drivers) != 1 || drivers[0] != wifiDriverDir {
		t.Fatalf("drivers = %v", drivers)
	}
	if got.WiFiLibDir != wifiLibDir || got.WiFiLDSymbol != wifiLDSymbol {
		t.Fatalf("wifi paths = %q %q", got.WiFiLibDir, got.WiFiLDSymbol)
	}
}

func TestTargetGuards(t *testing.T) {
	st := Load(nil, filepath.Join(t.TempDir(), "x.conf")) /* defaults to x86 */
	st.SetWiFi(true)
	if st.WiFi() {
		t.Fatal("wifi enabled on a target that cannot have it")
	}
	st.SetTarget("esp32c3")
	st.SetWiFi(true)
	st.SetNet(true)
	if !st.WiFi() || !st.Net() {
		t.Fatal("wifi/net did not enable on esp32c3")
	}
	st.SetTarget("rp2040")
	if st.WiFi() || st.Net() {
		t.Fatal("wifi/net survived a target change")
	}
}

func TestKeyDrivesState(t *testing.T) {
	path := filepath.Join(t.TempDir(), "x.conf")
	st := Load(nil, path)
	st.SetTarget("esp32c3")
	st.SetWiFi(true)
	a := &app{st: st}
	a.stack = []page{a.mainPage()}

	/* rows: 0 target, 1 kernel, 2 drivers, 3 IPv4, 4 float, 5 programs */
	a.cur = 3
	a.handle([]byte{' '})
	if !st.Net() {
		t.Fatal("space did not enable net")
	}

	/* WiFi is a driver now: turning it off must also turn the stack off. */
	a.push(a.driverPage())
	a.cur = 0 /* the WiFi row */
	a.handle([]byte{' '})
	if st.WiFi() || st.Net() {
		t.Fatal("net stayed on after the WiFi driver was disabled")
	}
	if !a.dirty {
		t.Fatal("edits did not mark the config dirty")
	}

	a.pop()
	a.cur = 4
	a.handle([]byte{' '})
	if !st.SoftMin() {
		t.Fatal("space did not enable softfloat-min")
	}
}

func TestSubmenus(t *testing.T) {
	path := filepath.Join(t.TempDir(), "x.conf")
	st := Load(nil, path)
	st.SetTarget("esp32c3")
	a := &app{st: st, progs: []string{"hello"}}
	a.stack = []page{a.mainPage()}

	/* Target submenu: pick rp2350. */
	a.push(a.targetPage())
	found := false
	for i, r := range a.curPage().rows {
		if r.label == "rp2350" {
			a.cur = i
			found = true
			break
		}
	}
	if !found {
		t.Fatal("rp2350 missing from the target list")
	}
	a.handle([]byte{'\r'})
	if st.Target != "rp2350" {
		t.Fatalf("target = %q", st.Target)
	}
	if len(a.stack) != 1 {
		t.Fatalf("target page did not close: depth %d", len(a.stack))
	}

	/* Kernel page: cycle the heap to the next value. */
	a.push(a.kernelPage())
	a.cur = 0
	before := st.HeapBytes
	a.handle([]byte{' '})
	if st.HeapBytes == before {
		t.Fatal("heap did not change")
	}

	/* And the thread library switch, which is the optional-code toggle. */
	found = false
	for i, r := range a.curPage().rows {
		if strings.Contains(r.label, "Threads") {
			a.cur = i
			found = true
			break
		}
	}
	if !found {
		t.Fatal("threads switch missing from the kernel page")
	}
	beforeBool := st.Threads
	a.handle([]byte{' '})
	if st.Threads == beforeBool {
		t.Fatal("threads switch did not change")
	}

	/* The trim switches: history is a choice, the filesystem a bool. */
	found = false
	for i, r := range a.curPage().rows {
		if strings.Contains(r.label, "Command history") {
			a.cur = i
			found = true
			break
		}
	}
	if !found {
		t.Fatal("history switch missing from the kernel page")
	}
	beforeHist := st.HistBytes
	a.handle([]byte{' '})
	if st.HistBytes == beforeHist {
		t.Fatal("history switch did not change")
	}

	found = false
	for i, r := range a.curPage().rows {
		if strings.Contains(r.label, "Read-only") {
			a.cur = i
			found = true
			break
		}
	}
	if !found {
		t.Fatal("read-only switch missing from the kernel page")
	}
	beforeRO := st.FSReadOnly
	a.handle([]byte{' '})
	if st.FSReadOnly == beforeRO {
		t.Fatal("read-only switch did not change")
	}

	/* Drivers page: turn off the display driver on rp2350. */
	a.push(a.driverPage())
	found = false
	for i, r := range a.curPage().rows {
		if strings.Contains(r.label, "display") {
			a.cur = i
			found = true
			break
		}
	}
	if !found {
		t.Fatal("display driver missing on rp2350")
	}
	a.handle([]byte{' '})
	if len(st.DriverExclude) == 0 {
		t.Fatal("driver exclude not recorded")
	}
}

func TestProgramPage(t *testing.T) {
	path := filepath.Join(t.TempDir(), "x.conf")
	st := Load(nil, path)
	a := &app{st: st, progs: []string{"hello", "net"}}
	a.stack = []page{a.mainPage()}
	a.push(a.programPage())
	if len(a.curPage().rows) == 0 {
		t.Skip("no user programs found")
	}
	name := a.curPage().rows[0].label
	a.handle([]byte{' '})
	if !st.Exclude[name] {
		t.Fatalf("%s was not excluded", name)
	}
	a.handle([]byte{' '})
	if st.Exclude[name] {
		t.Fatalf("%s stayed excluded", name)
	}
}

func visible(s string) string {
	var b []byte
	for i := 0; i < len(s); {
		if s[i] == 0x1b && i+1 < len(s) && s[i+1] == '[' {
			j := i + 2
			for j < len(s) && (s[j] < '@' || s[j] > '~') {
				j++
			}
			if j < len(s) {
				j++
			}
			i = j
			continue
		}
		b = append(b, s[i])
		i++
	}
	return string(b)
}

func TestRenderLayout(t *testing.T) {
	st := Load(nil, "tos.conf")
	st.HeapBytes = 8 << 10
	st.KStack = 8 << 10
	a := &app{st: st, progs: []string{"hello", "net"}}
	a.stack = []page{a.mainPage()}

	for _, size := range [][2]int{{80, 24}, {40, 10}, {120, 40}} {
		lines := a.render(size[0], size[1])
		if len(lines) != size[1] {
			t.Fatalf("%dx%d: %d lines", size[0], size[1], len(lines))
		}
		for i, line := range lines {
			if n := len(visible(line)); n != size[0] {
				t.Fatalf("%dx%d line %d width %d: %q", size[0], size[1], i, n, visible(line))
			}
		}
		if !strings.Contains(visible(lines[0]), "TinyOS configuration") {
			t.Fatalf("title missing: %q", visible(lines[0]))
		}
		found := false
		for _, line := range lines {
			if strings.Contains(visible(line), "heap 8K, stack 8K") {
				found = true
			}
		}
		if !found {
			t.Fatalf("kernel summary missing")
		}
	}

	/* The selected row is reverse-video and the program submenu gets a title. */
	a.stack = append(a.stack, a.programPage())
	lines := a.render(80, 24)
	if !strings.Contains(visible(lines[0]), "User programs") {
		t.Fatalf("submenu title missing: %q", visible(lines[0]))
	}
}
