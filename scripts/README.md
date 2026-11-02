# scripts

The emulator / QEMU test harness lives in `cmd/tostest` (Go), not here:

    tostest smoke    QEMU TCP-serial smoke (was scripts/smoke.ps1)
    tostest compat   cross-target compatibility (was scripts/compat.ps1)
    tostest emu      build/smoke the rv32emu-c submodule (was scripts/emu.py)
    tostest emutest  full functional pass on RV32EMU-C (was scripts/esp-c3/emutest.py)
    tostest bench    guest bench timing on RV32EMU-C / QEMU (was scripts/bench.py + benchtime.py)

Only `esp-c3/` remains here: **real-hardware** bring-up and acceptance for the
ESP32-C3 board.  It stays in Python on purpose — esptool and pyserial are the
ESP ecosystem standard, the chip can only be flashed through esptool's
download-mode protocol, and there is no Go equivalent worth depending on.
Everything a porter needs for build and CI is Go + a C toolchain; the board
scripts are the one documented exception (see README.md "环境准备").
