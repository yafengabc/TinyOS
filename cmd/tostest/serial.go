package main

import (
	"bytes"
	"fmt"
	"io"
	"net"
	"os/exec"
	"sync"
	"time"
)

func fmtSockAddr(port int) string { return fmt.Sprintf("127.0.0.1:%d", port) }

// pump drains an io.Reader on a goroutine and hands the caller chunks with a
// timeout, the same shape the old Python scripts used (a reader thread feeding
// a queue).  It never blocks the caller.
type pump struct {
	ch chan []byte
}

func newPump(r io.Reader) *pump {
	p := &pump{ch: make(chan []byte, 16)}
	go func() {
		buf := make([]byte, 65536)
		for {
			n, err := r.Read(buf)
			if n > 0 {
				b := make([]byte, n)
				copy(b, buf[:n])
				p.ch <- b
			}
			if err != nil {
				break
			}
		}
		close(p.ch)
	}()
	return p
}

// mergedPump reads several streams concurrently and forwards chunks in arrival
// order, matching what `2>&1` does for the emulator sessions.
func mergedPump(readers ...io.Reader) *pump {
	p := &pump{ch: make(chan []byte, 32)}
	go func() {
		inner := make(chan []byte, 64)
		var wg sync.WaitGroup
		for _, r := range readers {
			wg.Add(1)
			go func(r io.Reader) {
				defer wg.Done()
				buf := make([]byte, 65536)
				for {
					n, err := r.Read(buf)
					if n > 0 {
						b := make([]byte, n)
						copy(b, buf[:n])
						inner <- b
					}
					if err != nil {
						return
					}
				}
			}(r)
		}
		go func() { wg.Wait(); close(inner) }()
		for b := range inner {
			p.ch <- b
		}
		close(p.ch)
	}()
	return p
}

// read returns the next chunk, or nil on timeout.  ok is false when the stream
// has reached EOF (timeout keeps ok true).
func (p *pump) read(timeout time.Duration) ([]byte, bool) {
	if timeout < 0 {
		timeout = 0
	}
	t := time.NewTimer(timeout)
	defer t.Stop()
	select {
	case b, ok := <-p.ch:
		if !ok {
			return nil, false
		}
		return b, true
	case <-t.C:
		return nil, true
	}
}

// session wraps a subprocess with an input pipe and an accumulated output
// buffer.  guest output (and stderr when merged) is pumped asynchronously.
type session struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	pump   *pump
	buf    []byte
	mu     sync.Mutex
	eof    bool
	stderr *bytes.Buffer
}

func startSession(cmd *exec.Cmd, mergeStderr bool) (*session, error) {
	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, err
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, err
	}
	stderr, err := cmd.StderrPipe()
	if err != nil {
		return nil, err
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	s := &session{cmd: cmd, stdin: stdin}
	if mergeStderr {
		s.pump = mergedPump(stdout, stderr)
	} else {
		s.stderr = &bytes.Buffer{}
		go func() {
			buf := make([]byte, 65536)
			for {
				n, err := stderr.Read(buf)
				if n > 0 {
					s.stderr.Write(buf[:n])
				}
				if err != nil {
					return
				}
			}
		}()
		s.pump = newPump(stdout)
	}
	return s, nil
}

// read pulls the next chunk into the buffer.  Returns the chunk (nil on
// timeout/EOF); ok=false on EOF.
func (s *session) read(timeout time.Duration) ([]byte, bool) {
	b, ok := s.pump.read(timeout)
	if b != nil {
		s.mu.Lock()
		s.buf = append(s.buf, b...)
		s.mu.Unlock()
	}
	if !ok {
		s.eof = true
	}
	return b, ok
}

// waitFor keeps reading until needle is in the buffer or the deadline passes.
func (s *session) waitFor(needle []byte, timeout time.Duration) (bool, time.Duration) {
	start := time.Now()
	deadline := start.Add(timeout)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			break
		}
		if remaining > 200*time.Millisecond {
			remaining = 200 * time.Millisecond
		}
		_, ok := s.read(remaining)
		if !ok {
			break
		}
		s.mu.Lock()
		hit := bytes.Contains(s.buf, needle)
		s.mu.Unlock()
		if hit {
			return true, time.Since(start)
		}
	}
	return false, time.Since(start)
}

// drain keeps reading for the whole window, like the old Read-Available.
func (s *session) drain(timeout time.Duration) {
	end := time.Now().Add(timeout)
	for time.Now().Before(end) {
		remaining := time.Until(end)
		if remaining > 200*time.Millisecond {
			remaining = 200 * time.Millisecond
		}
		_, ok := s.read(remaining)
		if !ok {
			return
		}
	}
}

func (s *session) text() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return string(s.buf)
}

// waitForText keeps reading until pred(text) holds or the deadline passes.
func (s *session) waitForText(pred func(string) bool, timeout time.Duration) (bool, time.Duration) {
	start := time.Now()
	deadline := start.Add(timeout)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			break
		}
		if remaining > 200*time.Millisecond {
			remaining = 200 * time.Millisecond
		}
		_, ok := s.read(remaining)
		if !ok {
			break
		}
		if pred(s.text()) {
			return true, time.Since(start)
		}
	}
	return false, time.Since(start)
}

// mark / sinceMark bracket a slice of the accumulated output (for printing
// just what happened after a given point).
func (s *session) mark() int {
	s.mu.Lock()
	defer s.mu.Unlock()
	return len(s.buf)
}

func (s *session) sinceMark(mark int) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	if mark < len(s.buf) {
		return string(s.buf[mark:])
	}
	return ""
}

// tail returns the last n bytes of accumulated output.
func (s *session) tail(n int) string {
	s.mu.Lock()
	defer s.mu.Unlock()
	if len(s.buf) <= n {
		return string(s.buf)
	}
	return string(s.buf[len(s.buf)-n:])
}

func (s *session) send(data []byte) error {
	_, err := s.stdin.Write(data)
	return err
}

func (s *session) sendLine(line string, newline string) error {
	_, err := s.stdin.Write([]byte(line + newline))
	return err
}

func (s *session) close() {
	if s.cmd.Process != nil {
		_ = s.cmd.Process.Kill()
		_ = s.cmd.Wait()
	}
	_ = s.stdin.Close()
}

// socketConn is a pump over a TCP connection (the QEMU serial port).
type socketConn struct {
	conn net.Conn
	pump *pump
	buf  []byte
	mu   sync.Mutex
	eof  bool
}

func dialSerial(port int, timeout time.Duration) (*socketConn, error) {
	deadline := time.Now().Add(timeout)
	for {
		conn, err := net.DialTimeout("tcp", fmtSockAddr(port), 200*time.Millisecond)
		if err == nil {
			if tc, ok := conn.(*net.TCPConn); ok {
				_ = tc.SetNoDelay(true)
			}
			return &socketConn{conn: conn, pump: newPump(conn)}, nil
		}
		if time.Now().After(deadline) {
			return nil, err
		}
		time.Sleep(200 * time.Millisecond)
	}
}

func (sc *socketConn) read(timeout time.Duration) ([]byte, bool) {
	b, ok := sc.pump.read(timeout)
	if b != nil {
		sc.mu.Lock()
		sc.buf = append(sc.buf, b...)
		sc.mu.Unlock()
	}
	if !ok {
		sc.eof = true
	}
	return b, ok
}

func (sc *socketConn) waitFor(needle []byte, timeout time.Duration) (bool, time.Duration) {
	start := time.Now()
	deadline := start.Add(timeout)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			break
		}
		if remaining > 200*time.Millisecond {
			remaining = 200 * time.Millisecond
		}
		_, ok := sc.read(remaining)
		if !ok {
			break
		}
		sc.mu.Lock()
		hit := bytes.Contains(sc.buf, needle)
		sc.mu.Unlock()
		if hit {
			return true, time.Since(start)
		}
	}
	return false, time.Since(start)
}

func (sc *socketConn) drain(timeout time.Duration) {
	end := time.Now().Add(timeout)
	for time.Now().Before(end) {
		remaining := time.Until(end)
		if remaining > 200*time.Millisecond {
			remaining = 200 * time.Millisecond
		}
		_, ok := sc.read(remaining)
		if !ok {
			return
		}
	}
}

func (sc *socketConn) text() string {
	sc.mu.Lock()
	defer sc.mu.Unlock()
	return string(sc.buf)
}

func (sc *socketConn) send(line string) error {
	_, err := sc.conn.Write([]byte(line + "\n"))
	return err
}

func (sc *socketConn) close() {
	_ = sc.conn.Close()
}
