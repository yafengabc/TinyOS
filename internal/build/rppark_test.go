package build

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// The three RP boards share the whole park protocol now: the state, the
// ordering rules and the enter/exit sequences.  The rules and the loops live in
// rp_park.h; each board only supplies its hooks and calls RP_PARK_OWNER_TAKE /
// RP_PARK_PARKED_LOOP.  This pins both halves so a board cannot quietly grow its
// own copy of the globals, or go back to hand-rolling a loop that the header
// already owns (issue #17).
func TestRpParkUsesSharedState(t *testing.T) {
	root, err := FindRoot()
	if err != nil {
		t.Skip("not in the source tree")
	}

	// The ordering rules and the two sequences must exist in the header, in
	// exactly one place.
	header, err := os.ReadFile(filepath.Join(root, "board", "rp_park.h"))
	if err != nil {
		t.Fatal(err)
	}
	htext := string(header)
	for _, want := range []string{
		"rp_park_begin(", "rp_park_ack(", "RP_PARK_OWNER_TAKE(", "RP_PARK_PARKED_LOOP(",
	} {
		if !strings.Contains(htext, want) {
			t.Errorf("rp_park.h: no %s - the shared skeleton is missing", want)
		}
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
		// Both sequences come from the header now: the owner raises the
		// request through RP_PARK_OWNER_TAKE and the parked core spins through
		// RP_PARK_PARKED_LOOP.  A board that open-codes either is a regression.
		for _, want := range []string{"RP_PARK_OWNER_TAKE(&park", "RP_PARK_PARKED_LOOP(&park"} {
			if !strings.Contains(text, want) {
				t.Errorf("%s: no %s - the sequence went back to being hand-rolled", board, want)
			}
		}
		for _, gone := range []string{"++park.req", "park.ack = park.req", "rp_park_ack(&park"} {
			if strings.Contains(text, gone) {
				t.Errorf("%s: still open-codes %q instead of the shared sequence", board, gone)
			}
		}
	}
}
