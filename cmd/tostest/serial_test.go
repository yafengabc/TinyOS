package main

import (
	"net"
	"testing"
	"time"
)

// waitFor used to give up after a fixed window no matter what the guest was
// doing, so a command that prints steadily but slowly was cut off and the next
// command landed in the middle of its output.  That is the "lost command
// output" the rp2350 review thread hit in scripts/soak.py.
//
// These three cases pin the policy: steady progress extends the wait, silence
// does not, and a guest that never stops still returns.
func TestWaitForSlidesWhileOutputArrives(t *testing.T) {
	sc, w := testSocket(t)

	go func() {
		defer w.Close()
		for i := 0; i < 6; i++ {
			time.Sleep(50 * time.Millisecond)
			if _, err := w.Write([]byte("x")); err != nil {
				return
			}
		}
		w.Write([]byte("done"))
	}()

	start := time.Now()
	// 200 ms of window, 350 ms of output: without the sliding deadline this
	// misses, because the last byte arrives after the first deadline.
	ok, _ := sc.waitFor([]byte("done"), 200*time.Millisecond)
	if !ok {
		t.Fatalf("waitFor gave up after %v while the guest was still printing",
			time.Since(start))
	}
}

func TestWaitForStillGivesUpOnSilence(t *testing.T) {
	sc, w := testSocket(t)
	defer w.Close()

	start := time.Now()
	ok, took := sc.waitFor([]byte("never"), 150*time.Millisecond)
	if ok {
		t.Fatal("waitFor reported a match that never arrived")
	}
	if took < 150*time.Millisecond {
		t.Fatalf("returned after %v, before the window expired", took)
	}
	if elapsed := time.Since(start); elapsed > 500*time.Millisecond {
		t.Fatalf("silence took %v, the deadline should not have slid", elapsed)
	}
}

func TestWaitForCapsEndlessOutput(t *testing.T) {
	sc, w := testSocket(t)
	stop := make(chan struct{})
	defer close(stop)
	defer w.Close()

	go func() {
		for {
			select {
			case <-stop:
				return
			default:
			}
			if _, err := w.Write([]byte("y")); err != nil {
				return
			}
			time.Sleep(10 * time.Millisecond)
		}
	}()

	// 3x the window is the hard cap: a guest that never prints the needle
	// still comes back instead of hanging the test run.
	start := time.Now()
	ok, _ := sc.waitFor([]byte("never"), 100*time.Millisecond)
	if ok {
		t.Fatal("waitFor reported a match that never arrived")
	}
	if elapsed := time.Since(start); elapsed < 250*time.Millisecond ||
		elapsed > 1200*time.Millisecond {
		t.Fatalf("endless output returned after %v, want about 300ms", elapsed)
	}
}

// A prompt is a synchronisation point only when it belongs to the command
// that was just sent.  The driver used to search from markSent, so a prompt
// still in flight from the previous command satisfied the wait immediately;
// the next command then landed in this one's output (the lost soak cycle).
// waitForMark consumes the echo, so the stale prompt in front of it cannot be
// matched.
func TestWaitForMarkConsumesTheCommandEcho(t *testing.T) {
	sc, w := testSocket(t)

	sc.markSent()
	if _, err := w.Write([]byte("tos> cmd\r\n")); err != nil {
		t.Fatal(err)
	}
	if ok, _ := sc.waitForMark([]byte("cmd"), time.Second); !ok {
		t.Fatal("echo of the command was never seen")
	}
	// Only the stale prompt is in the buffer; the prompt after this
	// command's output has not arrived yet, so this must not match.
	if ok, _ := sc.waitFor([]byte("tos> "), 150*time.Millisecond); ok {
		t.Fatal("matched the prompt from before the command")
	}
	if _, err := w.Write([]byte("out\r\ntos> ")); err != nil {
		t.Fatal(err)
	}
	if ok, _ := sc.waitFor([]byte("tos> "), time.Second); !ok {
		t.Fatal("did not see the prompt after the command output")
	}
}

func testSocket(t *testing.T) (*socketConn, net.Conn) {
	t.Helper()
	client, server := net.Pipe()
	sc := &socketConn{conn: client, pump: newPump(client)}
	t.Cleanup(func() { client.Close() })
	return sc, server
}
