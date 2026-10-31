.PHONY: all build run headless clean doctor

all: build

build:
	go run ./cmd/tos build

run:
	go run ./cmd/tos run

headless:
	go run ./cmd/tos run -headless

doctor:
	go run ./cmd/tos doctor

clean:
	go run ./cmd/tos clean
