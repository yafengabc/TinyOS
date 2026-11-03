package main

// tosput - a Windows GUI that puts a file onto a TinyOS board over the
// console serial line.
//
// Everything interesting lives in sender.go (the protocol) and, on the
// other end of the wire, user/rx.c (the receiver).  This file is just the
// window: port picker, file picker, progress, log, two buttons.

import (
	"fmt"
	"os"
	"sync"
	"time"

	"github.com/lxn/walk"
	. "github.com/lxn/walk/declarative"
	"go.bug.st/serial"
)

type uiState struct {
	mu      sync.Mutex
	sending bool
	cancel  bool
}

func main() {
	// The CLI build (tosput-cli.exe) and `-cli` both run headless.
	if len(os.Args) > 1 && os.Args[1] == "-cli" || cliBuild() {
		os.Exit(runCli())
	}

	var (
		mw      *walk.MainWindow
		portsCb *walk.ComboBox
		baudCb  *walk.ComboBox
		localLe *walk.LineEdit
		remoteLe *walk.LineEdit
		pb      *walk.ProgressBar
		logTv   *walk.TextEdit
		sendBtn *walk.PushButton
		cancelB *walk.PushButton
	)
	st := &uiState{}

	var logMu sync.Mutex
	logf := func(format string, args ...interface{}) {
		logMu.Lock()
		defer logMu.Unlock()
		if logTv != nil {
			line := fmt.Sprintf("%s  "+format, append([]interface{}{time.Now().Format("15:04:05")}, args...)...)
			mw.Synchronize(func() {
				logTv.AppendText(line + "\r\n")
			})
		}
	}

	progress := func(done, total int64) {
		mw.Synchronize(func() {
			pb.SetRange(0, int(total))
			pb.SetValue(int(done))
		})
	}

	refreshPorts := func() {
		list, err := serial.GetPortsList()
		if err != nil {
			list = nil
		}
		mw.Synchronize(func() {
			portsCb.SetModel(list)
			if len(list) > 0 {
				portsCb.SetCurrentIndex(0)
			}
		})
	}

	startSend := func() {
		st.mu.Lock()
		if st.sending {
			st.mu.Unlock()
			return
		}
		st.sending, st.cancel = true, false
		st.mu.Unlock()

		go func() {
			defer func() {
				st.mu.Lock()
				st.sending = false
				st.mu.Unlock()
				mw.Synchronize(func() {
					sendBtn.SetEnabled(true)
					cancelB.SetEnabled(false)
				})
			}()

			port := portsCb.Text()
			if port == "" {
				logf("no port selected")
				return
			}
			local := localLe.Text()
			if local == "" {
				logf("no local file chosen")
				return
			}
			remote := remoteLe.Text()
			if remote == "" {
				remote = "/" + baseName(local)
			}

			baud := 115200
			fmt.Sscanf(baudCb.Text(), "%d", &baud)

			cancel := func() bool {
				st.mu.Lock()
				defer st.mu.Unlock()
				return st.cancel
			}
			s, err := OpenSender(port, baud, logf, progress, cancel)
			if err != nil {
				logf("open %s: %v", port, err)
				return
			}
			defer s.Close()

			mw.Synchronize(func() {
				sendBtn.SetEnabled(false)
				cancelB.SetEnabled(true)
			})
			t0 := time.Now()
			if err := s.SendFile(local, remote); err != nil {
				if err == errAborted {
					logf("aborted by user")
					s.Abort()
				} else {
					logf("FAILED: %v", err)
					s.Abort()
				}
				return
			}
			dt := time.Since(t0).Seconds()
			if dt > 0 {
				logf("done in %.1f s (%.1f KiB/s)", dt, float64(len(mustRead(local)))/1024.0/dt)
			}
		}()
	}

	err := MainWindow{
		AssignTo: &mw,
		Title:    "TinyOS file transfer (tosput)",
		MinSize:  Size{Width: 560, Height: 420},
		Layout:   VBox{},
		Children: []Widget{
			Composite{
				Layout: Grid{Columns: 2},
				Children: []Widget{
					Label{Text: "Port:"},
					Composite{
						Layout: HBox{MarginsZero: true},
						Children: []Widget{
							ComboBox{AssignTo: &portsCb, MinSize: Size{Width: 160}},
							PushButton{Text: "Refresh", OnClicked: func() { go refreshPorts() }},
						},
					},
					Label{Text: "Baud:"},
					ComboBox{AssignTo: &baudCb, Value: "115200",
						Model: []string{"9600", "57600", "115200", "230400", "460800"}},
					Label{Text: "Local file:"},
					Composite{
						Layout: HBox{MarginsZero: true},
						Children: []Widget{
							LineEdit{AssignTo: &localLe},
							PushButton{Text: "Browse...", OnClicked: func() {
								d := new(walk.FileDialog)
								d.Filter = "All files (*.*)|*.*"
								if ok, _ := d.ShowOpen(mw); ok {
									localLe.SetText(d.FilePath)
								}
							}},
						},
					},
					Label{Text: "Board path:"},
					LineEdit{AssignTo: &remoteLe, CueBanner: "/file.bin (default: local name)"},
				},
			},
			ProgressBar{AssignTo: &pb},
			Composite{
				Layout: HBox{MarginsZero: true},
				Children: []Widget{
					PushButton{AssignTo: &sendBtn, Text: "Send", OnClicked: startSend},
					PushButton{AssignTo: &cancelB, Text: "Cancel", Enabled: false, OnClicked: func() {
						st.mu.Lock()
						st.cancel = true
						st.mu.Unlock()
					}},
				},
			},
			TextEdit{AssignTo: &logTv, VScroll: true, ReadOnly: true},
		},
	}.Create()
	if err != nil {
		fmt.Fprintln(os.Stderr, "create window:", err)
		os.Exit(1)
	}

	go refreshPorts()
	logf("tosput ready - pick a port and a file, then Send")

	mw.Run()
}

func baseName(p string) string {
	for i := len(p) - 1; i >= 0; i-- {
		if p[i] == '\\' || p[i] == '/' {
			return p[i+1:]
		}
	}
	return p
}

func mustRead(p string) []byte {
	b, _ := os.ReadFile(p)
	return b
}
