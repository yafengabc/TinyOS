package main

// Sender side of the TinyOS console transfer protocol.
//
// The board runs user/rx.c; this file speaks its language over a plain
// 115200 8N1 serial port:
//
//	send "rx <path>\n"            wait for "READY"
//	send "D <seq> <len> <b64>\n"  wait for "ACK <seq>" (or "NAK" -> resend)
//	...
//	send "E <crc32hex>\n"         wait for "DONE <crc32hex> <total>"
//
// Chunks are 128 bytes: a D-line is then ~180 characters, which fits
// through the kernel's tick-polled UART path without pacing (the C3
// loses bytes past ~350 characters of unbroken burst).  Stop-and-wait is
// still the protocol: while rx is
// inside write() programming a flash block, the board cannot drain its
// tty queue, so the sender must be silent until the ACK comes back.

import (
	"bufio"
	"encoding/base64"
	"errors"
	"fmt"
	"hash/crc32"
	"io"
	"os"
	"strings"
	"time"

	"go.bug.st/serial"
)

const (
	chunkSize  = 512 // well under one tick of UART FIFO on the board; see
	// the NAK story in docs/PLATFORM-DESIGN.md §8.10
	readyWait  = 8 * time.Second
	ackWait    = 4 * time.Second
	ackRetries = 3
	promptWait = 5 * time.Second
)

var (
	errTimeout = errors.New("timeout")
	errAborted = errors.New("aborted")
)

// Sender owns the port and the line protocol.
type Sender struct {
	port     serial.Port
	rd       *bufio.Reader
	logf     func(format string, args ...interface{})
	progress func(done, total int64)
	cancel   func() bool // returns true when the user asked to abort
}

func OpenSender(name string, baud int,
	logf func(string, ...interface{}),
	progress func(int64, int64),
	cancel func() bool) (*Sender, error) {

	mode := &serial.Mode{
		BaudRate: baud,
		DataBits: 8,
		Parity:   serial.NoParity,
		StopBits: serial.OneStopBit,
	}
	p, err := serial.Open(name, mode)
	if err != nil {
		var pe *serial.PortError
		if errors.As(err, &pe) {
			return nil, fmt.Errorf("open %s: %s (code %v)", name, pe, pe.Code())
		}
		return nil, err
	}
	// Deassert both handshake lines immediately: the C3 devkit wires
	// EN/IO0 to them for auto-download, and the pulse from opening the
	// port would otherwise reboot the board out from under the transfer.
	_ = p.SetDTR(false)
	_ = p.SetRTS(false)
	// It resets anyway (the pulse happens inside the driver on open), so
	// give the boot a moment before the first protocol byte goes out.
	time.Sleep(1 * time.Second)
	_ = p.ResetInputBuffer()
	return &Sender{port: p, logf: logf, progress: progress, cancel: cancel}, nil
}

// writePaced sends a protocol line in small pieces with gaps between
// them.  The kernel polls the UART from a 100 Hz tick and the C3's RX
// FIFO is 128 bytes, so an uninterrupted burst longer than about 300
// characters overruns it: bytes are lost, the newline with them, and rx
// never sees a line at all.  96 bytes per piece under 12 ms keeps the
// FIFO below its depth at every point in time; the stop-and-wait ACK is
// what already bounds the rest.
func (s *Sender) writePaced(line []byte) error {
	const piece = 96
	const gap = 30 * time.Millisecond
	for off := 0; off < len(line); off += piece {
		end := off + piece
		if end > len(line) {
			end = len(line)
		}
		if _, err := s.port.Write(line[off:end]); err != nil {
			return err
		}
		if end < len(line) {
			time.Sleep(gap)
		}
	}
	return nil
}

// line reads one \n-terminated line with a deadline.  The trailing \r (if
// any) and \n are stripped.
func (s *Sender) line(timeout time.Duration) (string, error) {
	deadline := time.Now().Add(timeout)
	var acc []byte
	buf := make([]byte, 1)
	for {
		if s.cancel != nil && s.cancel() {
			return "", errAborted
		}
		_ = s.port.SetReadTimeout(50 * time.Millisecond)
		n, err := s.port.Read(buf)
		if err == nil && n > 0 {
			acc = append(acc, buf[0])
			if buf[0] == '\n' {
				return strings.TrimRight(string(acc), "\r\n"), nil
			}
			if len(acc) > 4096 {
				return "", fmt.Errorf("line too long: %q", acc)
			}
			continue
		}
		if time.Now().After(deadline) {
			if len(acc) > 0 {
				return "", fmt.Errorf("timeout with partial line %q", acc)
			}
			return "", errTimeout
		}
	}
}

// readUntil reads bytes until pattern shows up (no newline required - the
// shell's prompt is "tos> " and is not followed by one).
func (s *Sender) readUntil(pattern string, timeout time.Duration) (string, error) {
	deadline := time.Now().Add(timeout)
	var acc []byte
	buf := make([]byte, 1)
	for {
		if s.cancel != nil && s.cancel() {
			return "", errAborted
		}
		_ = s.port.SetReadTimeout(50 * time.Millisecond)
		n, err := s.port.Read(buf)
		if err == nil && n > 0 {
			acc = append(acc, buf[0])
			if strings.Contains(string(acc), pattern) {
				return string(acc), nil
			}
			if len(acc) > 8192 {
				acc = acc[4096:] // keep the tail, drop old noise
			}
			continue
		}
		if time.Now().After(deadline) {
			return string(acc), errTimeout
		}
	}
}

// drainPrompt swallows shell noise until a fresh "tos> " prompt: opening
// the port may interrupt a half-typed command, and every command echoes.
func (s *Sender) drainPrompt() error {
	// Opening the port pulses DTR/RTS and the C3 devkit's auto-download
	// circuit turns that into a chip reset: the first second of output is
	// the bootloader, not the shell.  Let it settle, then try for a
	// prompt three times, each attempt a bare newline plus a fresh wait.
	// Boot after the open-reset runs ~2-5 s (the fs self-test erases a
	// block), so one generous window beats three short ones.
	_ = s.port.ResetInputBuffer()
	if _, err := s.port.Write([]byte("\n")); err != nil {
		return err
	}
	if _, err := s.readUntil("tos> ", promptWait); err == nil {
		return nil
	} else if err == errAborted {
		return err
	}
	// One more try with a fresh newline before giving up.
	_, _ = s.port.Write([]byte("\n"))
	if _, err := s.readUntil("tos> ", promptWait); err == nil {
		return nil
	}
	return errors.New("no shell prompt (is the board running TinyOS?)")
}

// SendFile transfers the contents of localPath to remotePath on the board.
func (s *Sender) SendFile(localPath, remotePath string) error {
	f, err := os.Open(localPath)
	if err != nil {
		return err
	}
	defer f.Close()
	st, err := f.Stat()
	if err != nil {
		return err
	}
	total := st.Size()
	data, err := io.ReadAll(f)
	if err != nil {
		return err
	}
	sum := crc32.ChecksumIEEE(data)

	s.logf("sending %s -> %s (%d bytes, crc32 %08x)", localPath, remotePath, total, sum)
	if err := s.drainPrompt(); err != nil {
		return err
	}
	if _, err := s.port.Write([]byte(fmt.Sprintf("rx %s\n", remotePath))); err != nil {
		return err
	}

	// Skip the echoed command line, then expect READY.
	ready := false
	deadline := time.Now().Add(readyWait)
	var line string
	for time.Now().Before(deadline) && !ready {
		line, err = s.line(readyWait)
		if err != nil {
			if err == errAborted {
				return err
			}
			continue
		}
		switch {
		case strings.HasPrefix(line, "READY"):
			ready = true
		case strings.HasPrefix(line, "ERR"):
			return fmt.Errorf("board: %s", line)
		}
	}
	if !ready {
		return errors.New("no READY from rx (is rx in the image? flash the fs)")
	}

	// Raw (unpadded) base64: rx's decoder accepts only that, and the
	// padding '=' would be dead weight on the wire anyway.
	enc := base64.RawStdEncoding
	for seq, off := 0, 0; off < len(data); seq++ {
		n := len(data) - off
		if n > chunkSize {
			n = chunkSize
		}
		block := data[off : off+n]

		ok := false
		for attempt := 0; attempt < ackRetries && !ok; attempt++ {
			if s.cancel != nil && s.cancel() {
				return errAborted
			}
			payload := []byte(fmt.Sprintf("D %d %d %s\n", seq, n, enc.EncodeToString(block)))
			if err := s.writePaced(payload); err != nil {
				return err
			}
			deadline := time.Now().Add(ackWait)
			for time.Now().Before(deadline) {
				resp, err := s.line(ackWait)
				if err != nil {
					if err == errAborted {
						return err
					}
					break // retry the chunk
				}
				if resp == fmt.Sprintf("ACK %d", seq) {
					ok = true
					break
				}
				if strings.HasPrefix(resp, "NAK") || strings.HasPrefix(resp, "ERR") {
					break // retry the chunk
				}
				// anything else (stale shell noise) is ignored
			}
		}
		if !ok {
			return fmt.Errorf("chunk %d not acknowledged", seq)
		}
		off += n
		if s.progress != nil {
			s.progress(int64(off), total)
		}
	}

	if _, err := s.port.Write([]byte(fmt.Sprintf("E %08x\n", sum))); err != nil {
		return err
	}
	deadline = time.Now().Add(ackWait)
	for time.Now().Before(deadline) {
		line, err = s.line(ackWait)
		if err != nil {
			if err == errAborted {
				return err
			}
			continue
		}
		if strings.HasPrefix(line, "DONE") {
			var got, gotTotal uint32
			if _, err := fmt.Sscanf(line, "DONE %8x %d", &got, &gotTotal); err != nil {
				return fmt.Errorf("bad DONE line: %q", line)
			}
			if got != sum {
				return fmt.Errorf("crc mismatch: board %08x, host %08x", got, sum)
			}
			if int64(gotTotal) != total {
				return fmt.Errorf("size mismatch: board %d, host %d", gotTotal, total)
			}
			s.logf("verified: %d bytes, crc32 %08x", total, sum)
			return nil
		}
		if strings.HasPrefix(line, "ERR") {
			return fmt.Errorf("board: %s", line)
		}
	}
	return errors.New("no DONE from rx")
}

// Abort pokes ^C at the board and waits for the prompt: the tty kills the
// reading task on 0x03 even in raw mode.
func (s *Sender) Abort() {
	_, _ = s.port.Write([]byte{0x03})
	_, _ = s.port.Write([]byte("\n"))
	_ = s.drainPrompt()
}

func (s *Sender) Close() error {
	return s.port.Close()
}
