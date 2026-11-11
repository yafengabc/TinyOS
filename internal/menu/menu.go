package menu

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"tinyos/internal/build"
)

/* The editable subset of a build config, plus the canonical writer.
 *
 * The INI files stay the source of truth; menuconfig loads one, edits this
 * state and writes it back in a stable form.  Unknown features are preserved,
 * so a config written by hand does not lose anything the menu does not show. */

const (
	wifiDriverDir = "drivers/wifi/esp32c3"
	wifiLibDir    = "drivers/wifi/esp32c3/lib"
	wifiLDScript  = "drivers/wifi/esp32c3/lib/esp32c3.rom.ld"
	wifiLDSymbol  = "wifi_probe"
)

// Sources in user/ that are runtime libraries, not programs.
var librarySources = map[string]bool{
	"lib": true, "syscall": true, "aeabi": true, "softfloat32": true,
	"fix16": true, "floatprint": true, "faeabi": true, "thread": true,
}

type State struct {
	Path          string
	Target        string
	Features      map[string]bool
	Drivers       []string
	DriverExclude map[string]bool
	HeapBytes     int
	MaxProcs      int
	KStack        int
	Opt           string
	Threads       bool
	HistBytes     int
	ProcLite      bool
	DevNodes      bool
	Panel         bool
	FSReadOnly    bool
	Mailbox       bool
	WiFiLibDir    string
	WiFiLDScript  string
	WiFiLDSymbol  string
	Exclude       map[string]bool // program -> left out of the image
	SourceExclude map[string]bool
	LDExtra       []string
}

// Load copies a parsed config into the editable state.  A nil config (no file
// yet) starts from the defaults.
func Load(c *build.Config, path string) *State {
	s := &State{
		Path:          path,
		Target:        "x86",
		Features:      map[string]bool{},
		DriverExclude: map[string]bool{},
		Exclude:       map[string]bool{},
		SourceExclude: map[string]bool{},
		HistBytes:     512,
		DevNodes:      true,
		Panel:         true,
	}
	if c == nil {
		return s
	}
	if c.Target != "" {
		s.Target = c.Target
	}
	for k, v := range c.Features {
		s.Features[strings.ToLower(k)] = v
	}
	s.Drivers = append([]string{}, c.Drivers...)
	for _, d := range c.DriverExclude {
		s.DriverExclude[filepath.ToSlash(filepath.Clean(d))] = true
	}
	s.HeapBytes = c.HeapBytes
	s.MaxProcs = c.MaxProcs
	s.KStack = c.KStack
	s.Opt = c.Opt
	s.Threads = c.Threads
	s.HistBytes = c.HistBytes
	s.ProcLite = c.ProcLite
	s.DevNodes = c.DevNodes
	s.Panel = c.Panel
	s.FSReadOnly = c.FSReadOnly
	s.Mailbox = c.Mailbox
	s.WiFiLibDir = c.WiFiLibDir
	s.WiFiLDScript = c.WiFiLDScript
	s.WiFiLDSymbol = c.WiFiLDSymbol
	for k, v := range c.UserExclude {
		s.Exclude[k] = v
	}
	for k, v := range c.SourceExclude {
		s.SourceExclude[k] = v
	}
	s.LDExtra = append([]string{}, c.LDExtra...)
	return s
}

// DefaultFeature reports whether the selected target turns a driver on when
// the config says nothing.  RP2040's PWM/UART1/SPI/I2C use this so the board
// keeps working without a config, while menuconfig can still turn one off.
func (s *State) DefaultFeature(name string) bool {
	name = strings.ToLower(name)
	if t, ok := build.Targets[s.Target]; ok {
		for _, f := range t.DefaultFeatures {
			if strings.EqualFold(f, name) {
				return true
			}
		}
	}
	return false
}

func (s *State) Feature(name string) bool {
	name = strings.ToLower(name)
	if on, ok := s.Features[name]; ok {
		return on
	}
	return s.DefaultFeature(name)
}

// SetFeature stores an explicit choice, including false.  Deleting the key
// would mean "fall back to the target default", which is not what a user who
// just turned a default-on driver off asked for.
func (s *State) SetFeature(name string, on bool) {
	s.Features[strings.ToLower(name)] = on
}

func (s *State) WiFi() bool    { return s.Feature("wifi") }
func (s *State) Net() bool     { return s.Feature("net") }
func (s *State) SoftMin() bool { return s.Feature("softfloat-min") }

// DriverChoices lists the driver directories that apply to the current
// target: its board defaults plus whatever the config added.  The WiFi driver
// is controlled by the WiFi feature and is not shown here.
func (s *State) DriverChoices() []string {
	seen := map[string]bool{}
	var out []string
	add := func(d string) {
		d = filepath.ToSlash(filepath.Clean(d))
		if d == wifiDriverDir || seen[d] {
			return
		}
		seen[d] = true
		out = append(out, d)
	}
	if t, ok := build.Targets[s.Target]; ok {
		for _, d := range t.Drivers {
			add(d)
		}
	}
	for _, d := range s.Drivers {
		add(d)
	}
	sort.Strings(out)
	return out
}

// DriverRequired marks the directories the board cannot boot or talk to the
// host without: GPIO, the console serial/USB and the SPI bus the panel sits
// on.  Everything else (the LCD driver, config-added directories) is optional.
func DriverRequired(d string) bool {
	d = filepath.ToSlash(filepath.Clean(d))
	for _, p := range []string{"drivers/gpio/", "drivers/serial/",
		"drivers/usb/", "drivers/spi/"} {
		if strings.HasPrefix(d, p) {
			return true
		}
	}
	return false
}

func (s *State) target() build.Target { return build.Targets[s.Target] }

func (s *State) HeapDefault() int {
	if h := s.target().HeapBytes; h > 0 {
		return h
	}
	return 32768
}

func (s *State) KStackDefault() int {
	if s.target().Arch == "x86" {
		return 32768
	}
	return 16384
}

func (s *State) ProcsDefault() int {
	if p := s.target().MaxProcs; p > 0 {
		return p
	}
	return 16
}

func (s *State) heapActual() int {
	if s.HeapBytes > 0 {
		return s.HeapBytes
	}
	return s.HeapDefault()
}

func (s *State) kstackActual() int {
	if s.KStack > 0 {
		return s.KStack
	}
	return s.KStackDefault()
}

// SetTarget keeps the feature set consistent with what the target supports:
// WiFi only exists on the ESP32-C3 port, and net needs WiFi.
func (s *State) SetTarget(t string) {
	s.Target = t
	if !s.WiFiCapable() {
		s.SetFeature("wifi", false)
		s.SetFeature("net", false)
	}
}

func (s *State) WiFiCapable() bool { return s.Target == "esp32c3" }

func (s *State) SetWiFi(on bool) {
	if on && !s.WiFiCapable() {
		return
	}
	s.SetFeature("wifi", on)
	if !on {
		s.SetFeature("net", false)
	}
}

func (s *State) SetNet(on bool) {
	if on && !s.WiFi() {
		return
	}
	s.SetFeature("net", on)
}

// EffectiveDrivers is the [drivers] list this state implies: everything that
// was loaded, with the WiFi directory added or removed to match the feature.
func (s *State) EffectiveDrivers() []string {
	var out []string
	for _, d := range s.Drivers {
		if filepath.ToSlash(filepath.Clean(d)) == wifiDriverDir {
			continue
		}
		out = append(out, d)
	}
	if s.WiFi() && s.WiFiCapable() {
		out = append(out, wifiDriverDir)
	}
	sort.Strings(out)
	return out
}

// Programs lists the selectable user programs (user/*.c and user/<dir>).
func Programs(root string) []string {
	dir := filepath.Join(root, "user")
	entries, err := os.ReadDir(dir)
	if err != nil {
		return nil
	}
	var out []string
	for _, e := range entries {
		name := e.Name()
		if e.IsDir() {
			out = append(out, name)
			continue
		}
		if !strings.HasSuffix(name, ".c") {
			continue
		}
		base := strings.TrimSuffix(name, ".c")
		if librarySources[base] {
			continue
		}
		out = append(out, base)
	}
	sort.Strings(out)
	return out
}

// Render returns the canonical INI text.
func (s *State) Render() string {
	var b strings.Builder

	b.WriteString("# TinyOS build configuration -- written by `tos menuconfig`\n")
	b.WriteString("[build]\n")
	b.WriteString("target = " + s.Target + "\n")
	if s.HeapBytes > 0 {
		b.WriteString(fmt.Sprintf("heap = %d\n", s.HeapBytes))
	}
	if s.MaxProcs > 0 {
		b.WriteString(fmt.Sprintf("max_procs = %d\n", s.MaxProcs))
	}
	if s.KStack > 0 {
		b.WriteString(fmt.Sprintf("kstack = %d\n", s.KStack))
	}
	if s.Opt != "" {
		b.WriteString("opt = " + s.Opt + "\n")
	}
	if s.Threads {
		b.WriteString("threads = 1\n")
	}
	if s.HistBytes != 512 {
		b.WriteString(fmt.Sprintf("hist = %d\n", s.HistBytes))
	}
	if s.ProcLite {
		b.WriteString("proc_lite = 1\n")
	}
	if !s.DevNodes {
		b.WriteString("devnodes = 0\n")
	}
	if !s.Panel {
		b.WriteString("panel = 0\n")
	}
	if s.FSReadOnly {
		b.WriteString("fs_ro = 1\n")
	}
	if s.Mailbox {
		b.WriteString("mailbox = 1\n")
	}

	features := make([]string, 0, len(s.Features))
	for k, on := range s.Features {
		/* A false value that equals the target default adds nothing; the
		 * absence of the key already means "use the default". */
		if !on && !s.DefaultFeature(k) {
			continue
		}
		features = append(features, fmt.Sprintf("%s = %t",
			strings.ToLower(k), on))
	}
	sort.Strings(features)
	if len(features) > 0 {
		b.WriteString("\n[features]\n")
		for _, f := range features {
			b.WriteString(f + "\n")
		}
	}

	drivers := s.EffectiveDrivers()
	if len(drivers) > 0 || len(s.DriverExclude) > 0 {
		b.WriteString("\n[drivers]\n")
		seen := map[string]int{}
		for _, d := range drivers {
			key := filepath.Base(filepath.Clean(d))
			if n := seen[key]; n > 0 {
				key = fmt.Sprintf("%s%d", key, n)
			}
			seen[filepath.Base(filepath.Clean(d))]++
			b.WriteString(key + " = " + filepath.ToSlash(d) + "\n")
		}
		if len(s.DriverExclude) > 0 {
			names := make([]string, 0, len(s.DriverExclude))
			for d := range s.DriverExclude {
				names = append(names, filepath.ToSlash(d))
			}
			sort.Strings(names)
			b.WriteString("exclude = " + strings.Join(names, ",") + "\n")
		}
	}

	if s.WiFi() {
		lib, ld, sym := s.WiFiLibDir, s.WiFiLDScript, s.WiFiLDSymbol
		if lib == "" {
			lib = wifiLibDir
		}
		if ld == "" {
			ld = wifiLDScript
		}
		if sym == "" {
			sym = wifiLDSymbol
		}
		b.WriteString("\n[wifi]\n")
		b.WriteString("lib_dir = " + filepath.ToSlash(lib) + "\n")
		b.WriteString("ld_script = " + filepath.ToSlash(ld) + "\n")
		b.WriteString("ld_symbol = " + sym + "\n")
	}

	if len(s.Exclude) > 0 {
		names := make([]string, 0, len(s.Exclude))
		for n, off := range s.Exclude {
			if off {
				names = append(names, n)
			}
		}
		sort.Strings(names)
		if len(names) > 0 {
			b.WriteString("\n[user]\nexclude = " + strings.Join(names, ",") + "\n")
		}
	}

	if len(s.SourceExclude) > 0 {
		names := make([]string, 0, len(s.SourceExclude))
		for n := range s.SourceExclude {
			names = append(names, filepath.ToSlash(n))
		}
		sort.Strings(names)
		b.WriteString("\n[sources]\nexclude = " + strings.Join(names, ",") + "\n")
	}

	if len(s.LDExtra) > 0 {
		b.WriteString("\n[ld]\nextra = " + strings.Join(s.LDExtra, " ") + "\n")
	}

	return b.String()
}

// Save writes the canonical INI form to the state's path.
func (s *State) Save() error {
	return os.WriteFile(s.Path, []byte(s.Render()), 0o644)
}
