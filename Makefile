.PHONY: all build run headless clean doctor emu test-emu smoke compat bench

all: build

build:
	go run ./cmd/tos build

run:
	go run ./cmd/tos run

headless:
	go run ./cmd/tos run -headless

doctor:
	go run ./cmd/tos doctor

# Test harness: cmd/tostest (Go).  The old scripts/*.py and *.ps1 are gone;
# a fresh checkout needs only the C toolchain and Go.
emu:
	go run ./cmd/tostest emu build

test-emu:
	go run ./cmd/tostest emu test

smoke:
	go run ./cmd/tostest smoke

compat:
	go run ./cmd/tostest compat

bench:
	go run ./cmd/tostest bench

clean:
	go run ./cmd/tos clean
