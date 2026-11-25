package build

import (
	"os"
	"path/filepath"
	"regexp"
	"strings"
	"testing"
)

/* The negative cases are the point: an unknown key must fail loudly at the
 * line that wrote it.  These tests are deliberately built from strings, not
 * from the repository's own files, so deleting a key from a shipped config
 * cannot silently disable the check. */

func TestSchemaRejectsUnknownSection(t *testing.T) {
	err := validateINI("x.conf", "[build]\nheap = 8k\n\n[bogus]\nx = 1\n", configSchema)
	if err == nil {
		t.Fatal("unknown section was accepted")
	}
	if !strings.Contains(err.Error(), "x.conf:4") || !strings.Contains(err.Error(), "[bogus]") {
		t.Fatalf("error should name file:line and the section, got %q", err)
	}
}

func TestSchemaRejectsUnknownKey(t *testing.T) {
	// The key that shipped dead: pvm-float was read by nobody.
	err := validateINI("x.conf", "[features]\npvm-float = true\n", configSchema)
	if err == nil || !strings.Contains(err.Error(), "unknown feature") {
		t.Fatalf("unknown feature accepted: %v", err)
	}
	err = validateINI("x.conf", "[build]\nheap = 8k\nheep = 16k\n", configSchema)
	if err == nil || !strings.Contains(err.Error(), "x.conf:3") ||
		!strings.Contains(err.Error(), `"heep"`) {
		t.Fatalf("unknown build key accepted: %v", err)
	}
}

func TestSchemaRejectsUnknownListedFeature(t *testing.T) {
	// board.conf writes features as one list, not a table.
	err := validateINI("b/board.conf", "[board]\nschema = 1\nfeatures = pio, rirr\n", boardSchema)
	if err == nil || !strings.Contains(err.Error(), `"rirr"`) {
		t.Fatalf("unknown feature in a list was accepted: %v", err)
	}
}

func TestSchemaRejectsKeyInWrongFile(t *testing.T) {
	// window.* belongs to board.conf, not to an image config.
	err := validateINI("x.conf", "[window]\ngrain = 4k\n", configSchema)
	if err == nil || !strings.Contains(err.Error(), "unknown section [window]") {
		t.Fatalf("section from another layer was accepted: %v", err)
	}
}

func TestSchemasAcceptEveryShippedFile(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Fatal(err)
	}
	check := func(glob string, schema iniSchema) int {
		paths, _ := filepath.Glob(filepath.Join(root, glob))
		if len(paths) == 0 {
			t.Fatalf("no files matched %s", glob)
		}
		for _, p := range paths {
			data, err := os.ReadFile(p)
			if err != nil {
				t.Fatal(err)
			}
			if err := validateINI(p, string(data), schema); err != nil {
				t.Errorf("%v", err)
			}
			if _, err := parseINI(string(data)); err != nil {
				t.Errorf("%s: parseINI: %v", p, err)
			}
		}
		return len(paths)
	}
	n := check("configs/*.conf", configSchema)
	n += check("targets/*.conf", targetSchema)
	n += check("board/*/board.conf", boardSchema)
	if n < 50 {
		t.Fatalf("only %d files checked; the repository should have ~58", n)
	}
	t.Logf("validated %d shipped files", n)
}

// TestFeatureAllowlistCoversCode keeps the two sides in sync: a feature the
// code asks for must be allowed, and every allowed name must be used (or be
// one of the two that reach the C side through featureDefines()).
func TestFeatureAllowlistCoversCode(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Fatal(err)
	}
	re := regexp.MustCompile(`feature\("([a-z0-9_-]+)"\)`)
	used := map[string]bool{}
	files, _ := filepath.Glob(filepath.Join(root, "internal", "build", "*.go"))
	for _, p := range files {
		data, err := os.ReadFile(p)
		if err != nil {
			t.Fatal(err)
		}
		for _, m := range re.FindAllStringSubmatch(string(data), -1) {
			used[m[1]] = true
			if !featureAllowed(m[1]) {
				t.Errorf("%s: feature(%q) is not in featureNames", filepath.Base(p), m[1])
			}
		}
	}
	// Features with no feature() call site: they are consumed as defines.
	defineOnly := map[string]bool{
		"softfloat-min": true, // -DTOS_SOFTFLOAT_MIN in user/softfloat32.c
		"usb_pump":      true, // -DTOS_USB_PUMP: board/rp2350rv-generic only
		"wifi":          true, // build.go: also forced by the TOS_WIFI_LIB env
	}
	for _, f := range featureNames {
		if !used[f] && !defineOnly[f] {
			t.Errorf("featureNames has %q but no code reads it", f)
		}
	}
}
