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
	Threads bool
	// Kernel trimming.  Each one is a compile-time switch: the code (and,
	// for the .bss ones, the memory) is not in the image when it is off.  The
	// defaults are the full feature set, so an old config keeps its image.
	HistBytes  int  // command history pool; 0 = no recall
	ProcLite   bool // /proc keeps ps+uptime, drops mem/dev/lfs/stack/cpuinfo
	DevNodes   bool // /dev device classes and the GPIO/ADC syscalls
	Panel      bool // text mirror on a pixel panel (needs DevNodes to start)
	FSReadOnly bool // mount littlefs read-only; create/write/unlink fail
	// Mailbox is the global 4 x 256 B message pool (SYS_MSG_SEND/RECV).
	// Off by default: the code and the 1 KiB pool are only in the image when
	// a config asks for them.
	Mailbox       bool
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
		HistBytes:     512,
		DevNodes:      true,
		Panel:         true,
	}
	if s := sections["build"]; s != nil {
		bad := func(key string, err error) error {
			return fmt.Errorf("%s: [build] %s: %w", path, key, err)
		}
		c.Target = strings.TrimSpace(s["target"])
		if c.HeapBytes, err = parseSize(s["heap"]); err != nil {
			return nil, bad("heap", err)
		}
		if c.MaxProcs, err = parseSize(s["max_procs"]); err != nil {
			return nil, bad("max_procs", err)
		}
		if c.KStack, err = parseSize(s["kstack"]); err != nil {
			return nil, bad("kstack", err)
		}
		c.Opt = strings.TrimSpace(s["opt"])
		if c.Threads, err = parseOnOff(s["threads"]); err != nil {
			return nil, bad("threads", err)
		}
		if raw, ok := s["hist"]; ok {
			if c.HistBytes, err = parseSize(raw); err != nil {
				return nil, bad("hist", err)
			}
		}
		if raw, ok := s["proc_lite"]; ok {
			if c.ProcLite, err = parseOnOff(raw); err != nil {
				return nil, bad("proc_lite", err)
			}
		}
		if raw, ok := s["devnodes"]; ok {
			if c.DevNodes, err = parseOnOff(raw); err != nil {
				return nil, bad("devnodes", err)
			}
		}
		if raw, ok := s["panel"]; ok {
			if c.Panel, err = parseOnOff(raw); err != nil {
				return nil, bad("panel", err)
			}
		}
		if c.FSReadOnly, err = parseOnOff(s["fs_ro"]); err != nil {
			return nil, bad("fs_ro", err)
		}
		if c.Mailbox, err = parseOnOff(s["mailbox"]); err != nil {
			return nil, bad("mailbox", err)
		}
		if raw := s["features"]; raw != "" {
			for _, f := range splitList(raw) {
				c.Features[strings.ToLower(f)] = true
			}
		}
	}
	for name, on := range sections["features"] {
		on, err := truthy(on)
		if err != nil {
			return nil, fmt.Errorf("%s: [features] %s: %w", path, name, err)
		}
		c.Features[strings.ToLower(name)] = on
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

func truthy(s string) (bool, error) {
	switch strings.ToLower(strings.TrimSpace(s)) {
	case "1", "true", "yes", "on", "y":
		return true, nil
	case "0", "false", "no", "off", "n":
		return false, nil
	}
	return false, fmt.Errorf("invalid boolean %q", s)
}

// parseOnOff is the boolean form used by [build] keys such as threads.
// An absent key is off; a present but unrecognised value is an error rather
// than a silent "off", which used to hide a typo like devnodes = flase.
func parseOnOff(s string) (bool, error) {
	if strings.TrimSpace(s) == "" {
		return false, nil
	}
	return truthy(s)
}

// parseSize reads "81920", "80k" or "1m" as bytes (or a plain count for
// max_procs).  An absent key is 0 ("use the default"); a malformed or
// overflowing value is an error instead of silently selecting the default.
func parseSize(s string) (int, error) {
	s = strings.ToLower(strings.TrimSpace(s))
	if s == "" {
		return 0, nil
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
		return 0, fmt.Errorf("invalid size %q", s)
	}
	maxInt := int(^uint(0) >> 1)
	if n > maxInt/mul {
		return 0, fmt.Errorf("size %q overflows", s)
	}
	return n * mul, nil
}

// ApplyConfig copies the parsed values into the builder and resolves paths
// against the tree root.  A target already chosen on the command line wins
// over the config file.
func (b *Builder) ApplyConfig(c *Config) error {
	if c != nil {
		b.Config = c
		if b.Target.Name == "" && c.Target != "" {
			t, ok := Targets[c.Target]
			if !ok {
				return fmt.Errorf("config %s: unknown target %q", c.Path, c.Target)
			}
			b.Target = t
		}
	}

	/* Board defaults first: a target that cannot run WiFi programs must
	 * not get them just because no config file was passed.  A config's
	 * [user] exclude only adds to this set. */
	b.UserExclude = map[string]bool{}
	for _, n := range b.Target.UserExclude {
		b.UserExclude[n] = true
	}

	/* Same idea for driver features: RP2040 PWM/UART1/SPI/I2C are on by
	 * default, but the config can explicitly write "pwm = false" (which is
	 * what menuconfig saves) and the code disappears. */
	b.Features = map[string]bool{}
	for _, f := range b.Target.DefaultFeatures {
		b.Features[f] = true
	}
	if c == nil {
		return nil
	}
	for name, on := range c.Features {
		b.Features[name] = on
	}
	b.HeapBytes = c.HeapBytes
	b.MaxProcs = c.MaxProcs
	b.KStack = c.KStack
	b.Opt = c.Opt
	b.Threads = c.Threads
	b.HistBytes = c.HistBytes
	b.ProcLite = c.ProcLite
	b.DevNodes = c.DevNodes
	b.Panel = c.Panel
	b.FSReadOnly = c.FSReadOnly
	b.Mailbox = c.Mailbox
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
	for n := range c.UserExclude {
		b.UserExclude[n] = true
	}
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

// boolInt writes a boolean into a -D flag, where "absent" is not the same as
// "0" (the kernel headers test the number).
func boolInt(v bool) int {
	if v {
		return 1
	}
	return 0
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
