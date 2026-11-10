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
	"fix16": true, "floatprint": true, "faeabi": true,
}

type State struct {
	Path          string
	Target        string
	Features      map[string]bool
	Drivers       []string
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
		Exclude:       map[string]bool{},
		SourceExclude: map[string]bool{},
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

func (s *State) Feature(name string) bool { return s.Features[name] }

func (s *State) SetFeature(name string, on bool) {
	name = strings.ToLower(name)
	if on {
		s.Features[name] = true
	} else {
		delete(s.Features, name)
	}
}

func (s *State) WiFi() bool    { return s.Features["wifi"] }
func (s *State) Net() bool     { return s.Features["net"] }
func (s *State) SoftMin() bool { return s.Features["softfloat-min"] }

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

	features := make([]string, 0, len(s.Features))
	for k, on := range s.Features {
		if on {
			features = append(features, strings.ToLower(k))
		}
	}
	sort.Strings(features)
	if len(features) > 0 {
		b.WriteString("\n[features]\n")
		for _, f := range features {
			b.WriteString(f + " = true\n")
		}
	}

	drivers := s.EffectiveDrivers()
	if len(drivers) > 0 {
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
