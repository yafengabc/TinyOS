.PHONY: all build run headless clean doctor emu test-emu

all: build

build:
	go run ./cmd/tos build

run:
	go run ./cmd/tos run

headless:
	go run ./cmd/tos run -headless

doctor:
	go run ./cmd/tos doctor

# Standard test runtime: RV32EMU-C from the rv32emu-c/ submodule.
# `make emu` builds it into build/rv32emu-c.exe; `make test-emu` boots the
# esp32c3 kernel on it and pokes the shell. On Windows without make, run
# `python scripts/emu.py build|test` instead.
emu:
	python scripts/emu.py build

test-emu:
	python scripts/emu.py test

clean:
	go run ./cmd/tos clean
