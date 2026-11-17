package build

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The three RP boards share the park protocol state; the entry/exit sequences
// stay per-board until a bench session (OPEN-ITEMS 2f).  This pins the shared
// part so a board cannot quietly grow its own copy of the globals again.
func TestRpParkUsesSharedState(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}
	for _, board := range []string{"rp2040", "rp2350", "rp2350rv"} {
		body, err := os.ReadFile(filepath.Join(root, "board", board, "platform.c"))
		if err != nil {
			t.Fatal(err)
		}
		text := string(body)
		if !strings.Contains(text, `#include "rp_park.h"`) {
			t.Errorf("%s: does not include rp_park.h", board)
		}
		if !strings.Contains(text, "struct rp_park_state park = RP_PARK_STATE_INIT") {
			t.Errorf("%s: no shared park state", board)
		}
		for _, old := range []string{"g_flash_park", "g_park_req", "g_park_ack", "g_flash_parked"} {
			if strings.Contains(text, old) {
				t.Errorf("%s: still declares/uses %s", board, old)
			}
		}
	}
}
