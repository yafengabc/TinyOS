package build

import (
	"fmt"
	"sort"
	"strings"
)

/* Unknown keys are the quiet failure mode of an INI config: the parser hands
 * back a map, each loader reads the keys it knows, and everything else is
 * dropped on the floor.  configs/esp32c3-8k-float.conf shipped a
 * `pvm-float = true` that no code ever read; it did nothing for months and
 * nobody noticed until it was deleted.  A misspelled `[features]` name is
 * worse, because it *is* read somewhere: featureDefines() turns every feature
 * name into -DTOS_..., so `rir = ture` silently disables the toolchain.
 *
 * validateINI is the gate: every section and key has to be listed here, and
 * feature names come from one allowlist shared by configs and boards.  It is
 * deliberately a separate walk over the same text rather than a change to
 * parseINI's signature: the three loaders keep their map, and the validator
 * is the only place that has to know about line numbers. */

// featureNames is the allowlist.  Adding a feature means adding it here and
// in the code that consumes it; the allowlist test fails otherwise, which is
// the point (see strictschema_test.go).
//
// softfloat-min has no feature("softfloat-min") call site: it reaches the C
// side through featureDefines() (-DTOS_SOFTFLOAT_MIN) and user/softfloat32.c.
var featureNames = []string{
	"dma", "exec_mock", "i2c", "kthreads", "net", "pio", "pwm", "rir",
	"scripting", "softfloat-min", "spi", "uart1", "wifi",
}

func featureAllowed(name string) bool {
	for _, f := range featureNames {
		if f == name {
			return true
		}
	}
	return false
}

func featureList() string {
	return strings.Join(featureNames, ", ")
}

// iniSchema says which sections and keys a file may contain.  A nil key set
// means "any key is meaningful here": [drivers] is keyed by driver group
// names and the value is the directory, which ApplyConfig already checks.
type iniSchema struct {
	kind     string // config / board / target, for the error message
	sections map[string]map[string]bool
}

var configSchema = iniSchema{
	kind: "config",
	sections: map[string]map[string]bool{
		"build": {
			"target": true, "heap": true, "max_procs": true, "kstack": true,
			"opt": true, "xip_code_offset": true, "xip_map_base": true,
			"threads": true, "hist": true, "proc_lite": true, "devnodes": true,
			"boot_trace": true, "panel": true, "fs_ro": true, "mailbox": true,
			"features": true,
		},
		// Keyed by driver group ("wifi = drivers/wifi/esp32c3"); the value,
		// not the key, is what names a directory.
		"drivers":  nil,
		"features": nil, // feature names, checked against featureNames
		"wifi": {
			"lib_dir": true, "ld_script": true, "ld_symbol": true,
		},
		"ld":      {"extra": true},
		"user":    {"exclude": true},
		"sources": {"exclude": true},
	},
}

var boardSchema = iniSchema{
	kind: "board",
	sections: map[string]map[string]bool{
		"board": {
			"schema": true, "chip": true, "variant": true, "base": true,
			"peripherals": true, "fs_backing": true, "ramdisk_sectors": true,
			"xip_flash_base": true, "xip_map_base": true, "drivers": true,
			"features": true, "flash_bytes": true, "lfs_offset": true,
		},
		"user":   {"exclude": true},
		"window": {"grain": true, "stack": true, "guard": true, "stack_align": true},
	},
}

var targetSchema = iniSchema{
	kind: "target",
	sections: map[string]map[string]bool{
		"target": {
			"name": true, "aliases": true, "board": true, "arch": true,
			"march": true, "rcc_arch": true, "cpus": true, "heap": true,
			"kstack": true, "max_procs": true,
		},
	},
}

// validateINI rejects an unknown section, an unknown key, and a feature name
// that is not in featureNames.  Line numbers match what an editor shows.
func validateINI(path, text string, schema iniSchema) error {
	where := func(line int) string { return fmt.Sprintf("%s:%d", path, line) }
	section := ""
	for i, raw := range strings.Split(text, "\n") {
		line := strings.TrimSpace(strings.TrimSuffix(raw, "\r"))
		if line == "" || strings.HasPrefix(line, "#") || strings.HasPrefix(line, ";") {
			continue
		}
		if strings.HasPrefix(line, "[") {
			if !strings.HasSuffix(line, "]") {
				return fmt.Errorf("%s: unterminated section", where(i+1))
			}
			section = strings.ToLower(strings.TrimSpace(line[1 : len(line)-1]))
			if _, ok := schema.sections[section]; !ok {
				return fmt.Errorf("%s: unknown section [%s] in a %s file (allowed: %s)",
					where(i+1), section, schema.kind, schemaSections(schema))
			}
			continue
		}
		eq := strings.IndexByte(line, '=')
		if eq < 0 {
			return fmt.Errorf("%s: expected key = value", where(i+1))
		}
		key := strings.ToLower(strings.TrimSpace(line[:eq]))
		val := strings.TrimSpace(line[eq+1:])
		if section == "" {
			return fmt.Errorf("%s: key outside a section", where(i+1))
		}
		allowed := schema.sections[section]
		if allowed != nil && !allowed[key] {
			return fmt.Errorf("%s: unknown key %q in [%s] (allowed: %s)",
				where(i+1), key, section, schemaKeys(allowed))
		}
		if section == "features" && !featureAllowed(key) {
			return fmt.Errorf("%s: unknown feature %q in [features] (allowed: %s)",
				where(i+1), key, featureList())
		}
		if key == "features" && (section == "build" || section == "board") {
			for _, f := range splitList(val) {
				f = strings.ToLower(f)
				if !featureAllowed(f) {
					return fmt.Errorf("%s: unknown feature %q in [%s] features (allowed: %s)",
						where(i+1), f, section, featureList())
				}
			}
		}
	}
	return nil
}

func schemaSections(s iniSchema) string {
	names := make([]string, 0, len(s.sections))
	for n := range s.sections {
		names = append(names, n)
	}
	sort.Strings(names)
	return strings.Join(names, ", ")
}

func schemaKeys(keys map[string]bool) string {
	names := make([]string, 0, len(keys))
	for n := range keys {
		names = append(names, n)
	}
	sort.Strings(names)
	return strings.Join(names, ", ")
}
