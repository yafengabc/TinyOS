# TinyOS

[![Build flashable images](https://github.com/yafengabc/TinyOS/actions/workflows/images.yml/badge.svg)](https://github.com/yafengabc/TinyOS/actions/workflows/images.yml)
[![Latest release](https://img.shields.io/github/v/release/yafengabc/TinyOS)](https://github.com/yafengabc/TinyOS/releases)

A small preemptive 32-bit operating system written in **C and assembly**, with
one kernel and one userland shared by **12 build targets** — from a Cortex-M0+
with 264 KiB of RAM to QEMU and a PC. Programs run in **user mode** (x86 ring 3,
RISC-V U-mode, ARM Thread/PSP), each inside a window of its own, so a crashing
program takes only itself down.

It is not a Linux clone. It is a complete, readable system for microcontrollers:
scheduler, MMU-less memory protection, filesystem, shell, drivers, and a
build/test toolchain written in Go — small enough to read end to end.

## Why it is different

- **No SDK.** Every target — M0+, M33, Hazard3, RV32, i386 — is built by the
  same plain `clang` + `ld.lld` and nothing else. No pico-sdk install, no
  ESP-IDF install, no vendor build system: the boot2 stub, the ROM call tables,
  the pad registers and the pico-sdk-shaped facade TinyUSB asks for are all
  vendored under `third_party/` and `board/`. A new board is a directory, not
  an SDK.
- **A C compiler in the tree.** `goc32` (a submodule) turns one C file into
  the exact **LZ4TOS** blob `tos build` would put in littlefs — window hint,
  stack/heap asks and the LZ4 block included — for RV32IMC, Thumb-1/ARMv6-M,
  ARMv7-M and ARMv8-M. No libLLVM, no cgo, and no external toolchain: its
  assembler and linker live in `internal/as` and `internal/ld`. A `.tos`
  dropped into `build/fs-stage/bin/` is installed by the ramdisk rebuild.
  (goc32's code is larger than clang's; that is fine — release images and
  the prebuilt `msh` stay on clang, and goc32 is the fast single-file loop.)
  release images stay on clang.)
- **One kernel, one userland, 12 targets** — including both instruction sets of
  the same RP2350 silicon, and a PC.
- **Readable size** — the kernel is ~70 KiB of flash and ~45 KiB of RAM, and the
  whole system is a repository you can read end to end.

The only prebuilt binaries in the tree are the 256-byte RP2040 boot2 and the
optional ESP32-C3 WiFi blobs.

## At a glance

| Target | CPU | Board | RAM | Flash | State |
| --- | --- | --- | --- | --- | --- |
| `rpi-pico`, `rpi-pico-smp` | 2× Cortex-M0+ | Raspberry Pi Pico | 264 KiB | 2 MiB | on hardware |
| `rpi-pico2`, `rpi-pico2-smp` | 2× Cortex-M33 | Raspberry Pi Pico 2 | 520 KiB | 8 MiB | on hardware, **RP2350 gate** |
| `rpi-pico2-rv32`, `rpi-pico2-rv32-smp` | 2× Hazard3 (RISC-V) | Pico 2, other ISA | 520 KiB | 8 MiB | on hardware, **RP2350 gate** |
| `atk-dnr2350am`, `atk-dnr2350am-smp` | 2× Cortex-M33 | ATK-DNRP2350AM | 520 KiB | 4 MiB | reference board |
| `atk-dnr2350am-rv32`, `atk-dnr2350am-rv32-smp` | 2× Hazard3 (RISC-V) | ATK, other ISA | 520 KiB | 4 MiB | reference board |
| `esp32c3-generic` | RV32IMC | ESP32-C3 devkit | 400 KiB | 4 MiB | on hardware |
| `rv32`, `rv32smp`, `rv32nc` | RV32IMC(+A) | QEMU virt | 256 KiB heap | — | CI smoke + soak |
| `arm` | Cortex-M3 | QEMU mps2-an385 | — | — | CI smoke |
| `x86` | i386 | PC / QEMU | — | disk image | local only (see OPEN-ITEMS) |

`*smp` targets run both cores; the single-core targets of the same board are the
baseline used to tell "SMP-specific" from "ordinary" bugs.

## Highlights

- **Preemptive scheduler** — three priority levels, `sleep`/`prio`/`nice`,
  per-task CPU accounting, and **SMP** on three boards (RP2040, RP2350 M33,
  RP2350 Hazard3): per-core run queues, IPI-style wakeup, a flash-park
  handshake, and per-core SCB/MPU state.
- **User mode with real isolation, no MMU** — every task gets a window
  (image + stack + guard hole) enforced by PMSAv8 (M33), PMP TOR (C3), PMP
  NAPOT (Hazard3), or a software stack canary (M0+, x86). 4 KiB granularity, so
  a small tool costs 16 KiB and `msh` stays at 16 KiB.
- **A real userland** — PIE ELF programs loaded from littlefs: `msh` plus `sh`
  with pipes and redirection, 40+ tools (`ls dd df free ps cat wc edit gpio adc
  pwm spi i2c dma pio crash deep piperace`), a Python interpreter (`py`), and
  demos for threads, pipes, mailboxes and signals.
- **Filesystems that survive** — littlefs 2.x on flash, on an ATA disk (x86) or
  embedded; a block cache in front of the driver; a **single one-piece flash
  image** on the C3 (`esptool write-flash 0x0`).
- **Interfaces** — devfs (`/dev/gpio4`, `/dev/lcd0`, …) with an `ioctl` device
  model, procfs (`/proc/ps`, `/proc/mem`, …), pipes, signals, and a message
  mailbox.
- **Drivers** — UART (16550/CMSDK/PL011/C3/USJ), GPIO/ADC/PWM/SPI/I2C/DMA, PIO
  on all four RP targets, ST7789 panels, USB CDC (TinyUSB on RP, native USB on
  the C3), WiFi on the C3.
- **Built and gated in CI** — eleven flashable images per push, LLVM pinned to the
  version the sizes were measured with, a QEMU smoke + soak job, and releases
  published from tags. Every image ships its build configuration
  (`tinyos-<target>.build-config.txt`; `build/build-config.txt` in a local tree)
  saying what it was built from.

## Quick start

Requirements: **Go** (1.22+), **clang** and **ld.lld** on `PATH` — and nothing
else. There is no SDK to install: everything the boards need beyond that is in
the tree.

```sh
# Build any target
go run ./cmd/tos build -target atk-dnr2350am        # -> build/tinyos-atk-dnr2350am.uf2
go run ./cmd/tos build -target esp32c3-generic       # -> build/tinyos-esp32c3-generic.bin + -full.bin
go run ./cmd/tos build all                   # every target in targets/*.conf (15 images)

# RP2040 / RP2350: hold BOOTSEL, plug in USB, copy the .uf2 to the drive
cp build/tinyos-atk-dnr2350am.uf2 /media/$USER/RP2350/

# ESP32-C3: one write, kernel + filesystem together
python scripts/esp-c3/flash.py --full -p /dev/ttyACM0

# QEMU: boot and drive the shell
go run ./cmd/tostest smoke -target rv32
```

The shell that comes up is `msh`; try `ls`, `cat readme.txt`, `ps`, `free`,
`echo hi > /f`, `cat /f | wc`, `deep 3`, `prot stack`, `py demo.py`.

## The fast loop

A program is just a file in littlefs, so a one-line change does not need a new
300 KiB filesystem image — or a reflash:

```sh
goc32 user/foo.c -target atk-dnr2350am -o build/foo.tos   # C -> LZ4TOS blob
tosput-cli COM14 build/foo.tos /bin/foo            # base64 + CRC32 over the console
tos> foo
```

`tosput-cli` speaks the same line protocol as the on-board `rx` (raw mode,
stop-and-wait, CRC32 over what reached flash), so it works on any board with a
console and needs no kernel support beyond `ttyraw()`. See
[`docs/FILE-TRANSFER.md`](docs/FILE-TRANSFER.md) for the frame format and why
stop-and-wait is mandatory on a UART with no flow control.

The feature defines from the last `tos build` (`TOS_THREADS`, `TOS_MAILBOX`,
`TOS_NET`, ...) are reused automatically, and the thread/kthread runtimes are
linked when the build config enables them. clang still produces smaller code
than `goc32`, so release images stay with clang; the loop above is for the
ten-second cycle.

## Testing

```sh
go run ./cmd/tostest smoke -target rv32     # QEMU: boot + a command script
python scripts/esp-c3/accept.py -p COM4     # ESP32-C3: 24 acceptance checks
python scripts/rp2350/accept33.py --image atk-dnr2350am-rv32-smp --uf2 build/tinyos-atk-dnr2350am-rv32-smp.uf2
python scripts/soak.py --cycles 8           # exec/heap/segment churn, free must not drift
```

Hardware results live in [`docs/HISTORY.md`](docs/HISTORY.md): ESP32-C3 24/24,
RP2350 (M33) 16/16 SMP with `smpbench` 1.95x, RP2350 (Hazard3) 17/17 SMP with
2.00x, PIO 17/17 on both RP2350 images and on RP2040, RP2040 §30.3 window/guard
acceptance, RP2040 SMP `smpbench` 2.04x, and 1000 rounds of `dd`+`rm`.

## Documentation

| Document | What it covers |
| --- | --- |
| [`docs/GUIDE.zh-CN.md`](docs/GUIDE.zh-CN.md) | The long manual: per-board usage, memory layout, syscall ABI (Chinese) |
| [`docs/TESTING.md`](docs/TESTING.md) | Regression rules every agent must follow: tiers, commands, evidence (Chinese) |
| [`AGENTS.md`](AGENTS.md) | Normative agent rules: testing, safety, hardware |
| [`docs/PLATFORM-DESIGN.md`](docs/PLATFORM-DESIGN.md) | Porting contract, trap/user-mode entry, PMP/MPU layouts |
| [`docs/REAL-THREADS.md`](docs/REAL-THREADS.md) | The kthreads design and its SMP rules |
| [`docs/SHELL-GUIDE.md`](docs/SHELL-GUIDE.md) | Shell, tools and the userland API |
| [`docs/PIO.md`](docs/PIO.md) | The PIO loader and its console |
| [`docs/CI.md`](docs/CI.md) | Images, releases, the QEMU gate, toolchain pinning |
| [`docs/OPEN-ITEMS.md`](docs/OPEN-ITEMS.md) | What is still open, right now |
| [`docs/HISTORY.md`](docs/HISTORY.md) | Every closed item, with the measurement that closed it |
| [`docs/NUTTX-COMPARE.md`](docs/NUTTX-COMPARE.md) | NuttX/Zephyr comparison notes |

## Known limits

- **x86 has no paging** — privilege separation only; no per-task memory
  protection (same shape as the M0+ port before its software guard).
- **Cortex-M33 `deep 6`** still faults: exception entry needs a writable landing
  strip below the stack and only 2.5 KiB of the guard hole is left writable.
- **RP2040 USB CDC** is still being brought up; the UART console is the way in.
- No MMU anywhere: "protection" means a window per task, not virtual memory.

## Repository layout

```
kernel/     scheduler, processes, tty/console, littlefs glue, devfs, procfs
arch/       x86, rv32 and arm ports (trap entry, user mode, PMP/MPU)
board/      one directory per board: clocks, console, layout, flash
drivers/    uart, gpio, adc, pwm, spi, i2c, dma, pio, usb, wifi, display
user/       the userland: msh, sh, 40+ tools, libc, a Python interpreter
cmd/        tos (build/flash/package) and tostest (QEMU harness)
internal/   build system, image packers, filesystem image builder
docs/       design, guides and the open/history logs
```

## Related projects

- [goc32](https://github.com/yafengabc/goc32) — an experimental C compiler for
  the userland (RV32/Thumb), not part of the default build.
- [RV32EMU](https://github.com/yafengabc/RV32EMU) and
  [RV32EMU-C](https://github.com/yafengabc/RV32EMU-C) — an ESP32-C3 emulator in
  Go and in C, both able to boot TinyOS images.
