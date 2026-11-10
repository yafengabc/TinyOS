package build

import (
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
)

// Config is a small INI-style build configuration.  Keeping the parser in
// this package avoids pulling a TOML dependency into the toolchain; the
// format is deliberately flat:
//
//	[build]
//	target = esp32c3
//
//	[features]
//	wifi = true
//	net  = true
//
//	[drivers]
//	wifi = drivers/wifi/esp32c3
//
//	[wifi]
//	lib_dir   = drivers/wifi/esp32c3/lib
//	ld_script = drivers/wifi/esp32c3/lib/esp32c3.rom.ld
//	ld_symbol = wifi_probe
//
//	[user]
//	exclude = wifiscan,wificonn,ping,net,dhcp
//
// Relative paths are resolved against the source tree root.
type Config struct {
	Path          string
	Target        string
	Features      map[string]bool
	Drivers       []string
	DriverExclude []string
	// Kernel knobs.  Zero means "use the target's default".
	HeapBytes int
	MaxProcs  int
	KStack    int
	Opt       string
	// Threads compiles the cooperative user-space thread library (user/
	// thread.c and its per-arch switch) into every program.  Off by default:
	// a program that does not call thread_create() pays nothing either way,
	// but the flag is the switch the images are reproducible from.
	Threads       bool
	WiFiLibDir    string
	WiFiLDScript  string
	WiFiLDSymbol  string
	LDExtra       []string
	UserExclude   map[string]bool
	SourceExclude map[string]bool
}

func LoadConfig(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	sections, err := parseINI(string(data))
	if err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	c := &Config{
		Path:          path,
		Features:      map[string]bool{},
		UserExclude:   map[string]bool{},
		SourceExclude: map[string]bool{},
	}
	if s := sections["build"]; s != nil {
		c.Target = strings.TrimSpace(s["target"])
		c.HeapBytes = parseSize(s["heap"])
		c.MaxProcs = parseSize(s["max_procs"])
		c.KStack = parseSize(s["kstack"])
		c.Opt = strings.TrimSpace(s["opt"])
		c.Threads = parseOnOff(s["threads"])
		if raw := s["features"]; raw != "" {
			for _, f := range splitList(raw) {
				c.Features[strings.ToLower(f)] = true
			}
		}
	}
	for name, on := range sections["features"] {
		c.Features[strings.ToLower(name)] = truthy(on)
	}
	if s := sections["drivers"]; s != nil {
		var names []string
		for name := range s {
			names = append(names, name)
		}
		sort.Strings(names)
		for _, name := range names {
			if name == "exclude" {
				c.DriverExclude = append(c.DriverExclude, splitList(s[name])...)
				continue
			}
			for _, d := range splitList(s[name]) {
				if d != "" {
					c.Drivers = append(c.Drivers, d)
				}
			}
		}
	}
	if s := sections["wifi"]; s != nil {
		c.WiFiLibDir = strings.TrimSpace(s["lib_dir"])
		c.WiFiLDScript = strings.TrimSpace(s["ld_script"])
		c.WiFiLDSymbol = strings.TrimSpace(s["ld_symbol"])
	}
	if s := sections["ld"]; s != nil {
		c.LDExtra = strings.Fields(s["extra"])
	}
	if s := sections["user"]; s != nil {
		for _, n := range splitList(s["exclude"]) {
			c.UserExclude[n] = true
		}
	}
	if s := sections["sources"]; s != nil {
		for _, n := range splitList(s["exclude"]) {
			c.SourceExclude[filepath.ToSlash(n)] = true
		}
	}
	return c, nil
}

func parseINI(text string) (map[string]map[string]string, error) {
	out := map[string]map[string]string{}
	section := ""
	for i, line := range strings.Split(text, "\n") {
		line = strings.TrimSpace(strings.TrimSuffix(line, "\r"))
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
			continue
		}
		if strings.HasPrefix(line, "[") {
			if !strings.HasSuffix(line, "]") {
				return nil, fmt.Errorf("line %d: unterminated section", i+1)
			}
			section = strings.ToLower(strings.TrimSpace(line[1 : len(line)-1]))
			if out[section] == nil {
				out[section] = map[string]string{}
			}
			continue
		}
		eq := strings.IndexByte(line, '=')
		if eq < 0 {
			return nil, fmt.Errorf("line %d: expected key = value", i+1)
		}
		key := strings.ToLower(strings.TrimSpace(line[:eq]))
		val := strings.TrimSpace(line[eq+1:])
		if section == "" {
			return nil, fmt.Errorf("line %d: key outside a section", i+1)
		}
		out[section][key] = val
	}
	return out, nil
}

func splitList(s string) []string {
	s = strings.ReplaceAll(s, ";", ",")
	s = strings.ReplaceAll(s, " ", ",")
	parts := strings.Split(s, ",")
	out := parts[:0]
	for _, p := range parts {
		p = strings.TrimSpace(p)
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}

func truthy(s string) bool {
	switch strings.ToLower(strings.TrimSpace(s)) {
	case "1", "true", "yes", "on", "y":
		return true
	}
	return false
}

// parseOnOff is the boolean form used by [build] keys such as threads.
// Absent or unrecognised is off, which is the default for optional code.
func parseOnOff(s string) bool {
	switch strings.ToLower(strings.TrimSpace(s)) {
	case "1", "true", "yes", "on":
		return true
	}
	return false
}

// parseSize reads "81920", "80k" or "1m" as bytes (or a plain count for
// max_procs).  Anything unparsable is 0, which means "use the default".
func parseSize(s string) int {
	s = strings.ToLower(strings.TrimSpace(s))
	if s == "" {
		return 0
	}
	mul := 1
	switch {
	case strings.HasSuffix(s, "k"):
		mul = 1024
		s = s[:len(s)-1]
	case strings.HasSuffix(s, "m"):
		mul = 1024 * 1024
		s = s[:len(s)-1]
	}
	n, err := strconv.Atoi(strings.TrimSpace(s))
	if err != nil || n < 0 {
		return 0
	}
	return n * mul
}

// ApplyConfig copies the parsed values into the builder and resolves paths
// against the tree root.  A target already chosen on the command line wins
// over the config file.
func (b *Builder) ApplyConfig(c *Config) error {
	if c == nil {
		return nil
	}
	b.Config = c
	if b.Target.Name == "" && c.Target != "" {
		t, ok := Targets[c.Target]
		if !ok {
			return fmt.Errorf("config %s: unknown target %q", c.Path, c.Target)
		}
		b.Target = t
	}
	b.Features = c.Features
	b.HeapBytes = c.HeapBytes
	b.MaxProcs = c.MaxProcs
	b.KStack = c.KStack
	b.Opt = c.Opt
	b.Threads = c.Threads
	b.ensureDriverDirs()
	for _, d := range c.Drivers {
		p := b.abs(d)
		if st, err := os.Stat(p); err != nil || !st.IsDir() {
			return fmt.Errorf("config %s: driver directory %q not found", c.Path, d)
		}
		if !containsString(b.DriverDirs, p) {
			b.DriverDirs = append(b.DriverDirs, p)
		}
	}
	if len(c.DriverExclude) > 0 {
		drop := map[string]bool{}
		for _, d := range c.DriverExclude {
			drop[filepath.Clean(b.abs(d))] = true
		}
		kept := b.DriverDirs[:0]
		for _, d := range b.DriverDirs {
			if !drop[filepath.Clean(d)] {
				kept = append(kept, d)
			}
		}
		b.DriverDirs = kept
	}
	b.UserExclude = c.UserExclude
	b.SourceExclude = map[string]bool{}
	for s := range c.SourceExclude {
		b.SourceExclude[filepath.ToSlash(b.abs(s))] = true
	}
	b.WiFiLibDir = b.abs(c.WiFiLibDir)
	b.WiFiLDScript = b.abs(c.WiFiLDScript)
	if b.WiFiLibDir != "" {
		if st, err := os.Stat(b.WiFiLibDir); err != nil || !st.IsDir() {
			return fmt.Errorf("config %s: wifi lib_dir %q not found", c.Path, c.WiFiLibDir)
		}
	}
	if b.WiFiLDScript != "" {
		if _, err := os.Stat(b.WiFiLDScript); err != nil {
			return fmt.Errorf("config %s: wifi ld_script %q not found", c.Path, c.WiFiLDScript)
		}
	}
	b.WiFiLDSymbol = c.WiFiLDSymbol
	b.LDExtra = c.LDExtra
	return nil
}

func (b *Builder) abs(p string) string {
	if p == "" {
		return ""
	}
	if filepath.IsAbs(p) {
		return filepath.Clean(p)
	}
	return filepath.Join(b.Root, filepath.FromSlash(p))
}

// featureDefines turns [features] into -DTOS_<NAME> flags.
func (b *Builder) featureDefines() []string {
	var defs []string
	for name, on := range b.Features {
		if !on {
			continue
		}
		var n strings.Builder
		for _, r := range strings.ToUpper(name) {
			if (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '_' {
				n.WriteRune(r)
			} else {
				n.WriteByte('_')
			}
		}
		defs = append(defs, "-DTOS_"+n.String())
	}
	sort.Strings(defs)
	return defs
}

func (b *Builder) feature(name string) bool {
	return b.Features[name]
}

// ensureDriverDirs seeds the list with the target's board-default drivers
// once.  A config's [drivers] then appends to it, so the board's console,
// GPIO etc. stay available even when the config only adds WiFi.
func (b *Builder) ensureDriverDirs() {
	if b.driverDirsInit {
		return
	}
	b.driverDirsInit = true
	for _, d := range b.Target.Drivers {
		b.DriverDirs = append(b.DriverDirs, b.abs(d))
	}
}

// kernelLDArgs is the extra linker configuration selected by the build
// config: the WiFi ROM symbol script and its keep-alive root, plus any
// free-form [ld] extra flags.
func (b *Builder) kernelLDArgs() []string {
	args := append([]string(nil), b.LDExtra...)
	if b.feature("wifi") || os.Getenv("TOS_WIFI_LIB") != "" {
		if b.WiFiLDScript != "" {
			args = append(args, "-T", b.WiFiLDScript)
		}
		if b.WiFiLDSymbol != "" {
			args = append(args, "-u", b.WiFiLDSymbol)
		}
	}
	return args
}

func containsString(list []string, want string) bool {
	for _, s := range list {
		if s == want {
			return true
		}
	}
	return false
}

func hasFileInDirs(dirs []string, name string) bool {
	for _, d := range dirs {
		if _, err := os.Stat(filepath.Join(d, name)); err == nil {
			return true
		}
	}
	return false
}
