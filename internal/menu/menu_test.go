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
	a := &app{st: st}
	a.stack = []page{a.mainPage()}

	/* rows: 0 target, 1 kernel, 2 drivers, 3 wifi, 4 net, 5 float, 6 programs */
	a.cur = 3
	a.handle([]byte{' '}) /* toggle wifi */
	if !st.WiFi() {
		t.Fatal("space did not enable wifi")
	}
	a.cur = 4
	a.handle([]byte{' '})
	if !st.Net() {
		t.Fatal("space did not enable net")
	}
	a.cur = 3
	a.handle([]byte{' '}) /* wifi off implies net off */
	if st.WiFi() || st.Net() {
		t.Fatal("net stayed on after wifi was disabled")
	}
	if !a.dirty {
		t.Fatal("edits did not mark the config dirty")
	}

	a.cur = 5
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
	}

	/* The selected row is reverse-video and the program submenu gets a title. */
	a.stack = append(a.stack, a.programPage())
	lines := a.render(80, 24)
	if !strings.Contains(visible(lines[0]), "User programs") {
		t.Fatalf("submenu title missing: %q", visible(lines[0]))
	}
}
