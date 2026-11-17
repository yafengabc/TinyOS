package build

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The three RP boards share the park protocol state; the loops stay per-board
// until a bench session (#17).  This pins both halves so a board cannot quietly
// grow its own copy of the globals, or go back to hand-rolling the ordering
// that rp_park_begin()/rp_park_ack() own.
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
		// The ordering rules live in the header, not in each board: a request
		// is raised with rp_park_begin() and acknowledged with rp_park_ack(),
		// both of which carry the barrier in the right place.
		for _, want := range []string{"rp_park_begin(&park", "rp_park_ack(&park"} {
			if !strings.Contains(text, want) {
				t.Errorf("%s: no %s - the handshake went back to being hand-rolled", board, want)
			}
		}
		for _, gone := range []string{"++park.req", "park.ack = park.req"} {
			if strings.Contains(text, gone) {
				t.Errorf("%s: still open-codes %q instead of the shared helper", board, gone)
			}
		}
	}
}
