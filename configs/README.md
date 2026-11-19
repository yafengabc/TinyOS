# Build configuration

`tos build` reads `tos.conf` at the tree root by default.  Pass
`-config configs/<name>.conf` (or set `TOS_CONFIG`) to choose another file.
`-target` still overrides `[build] target`.

`tos menuconfig [-config <file>]` edits a config interactively: arrows move,
space toggles or cycles, enter opens a submenu, `s` saves, `q` quits.  It only
exposes the common knobs (target, WiFi/net, soft-float variant, user
programs); anything else in the file is preserved.  `-print` writes the
canonical INI to stdout and exits, without the UI.

The format is flat INI: sections, `key = value`, `#`/`;` comments.  Relative
paths are resolved against the tree root.

```ini
[build]
target = esp32c3-generic

[features]
wifi = true
net = true

[drivers]
wifi = drivers/wifi/esp32c3

[wifi]
lib_dir = drivers/wifi/esp32c3/lib
ld_script = drivers/wifi/esp32c3/lib/esp32c3.rom.ld
ld_symbol = wifi_probe

[user]
exclude = ping,net,dhcp
```

- `[build] target` picks an entry from `internal/build/build.go`'s `Targets`.
- `[build]` also takes kernel knobs, all optional and defaulting to the
  target's value: `heap = 81920` (kernel arena in bytes, `80k` works too),
  `max_procs = 8` (process table slots), `kstack = 16384` (per-CPU kernel
  stack, 8192/16384/32768; the linker pool and `kernel/cpu.h` read the same
  define) and `opt = -Oz` (compiler optimisation; `TOS_OPT` still wins).
  `boot_trace = 0` compiles the boot-progress traces and the `TOS_DIAG`
  diagnostics out in one step (the menuconfig item is "Boot traces +
  diagnostics"); the default is on because `scripts/rp2350/accept33.py` reads
  the `[A]`..`[E]` markers.  `configs/release.conf` is the quiet file to pass
  for a shipping image.
- `xip_code_offset` and `xip_map_base` (XIP boards only) split the code
  offset from the 64 KiB MMU mapping page; `xip_map_base` must be 64 KiB
  aligned.  The ESP32-C3 board defaults to `xip_flash_base = 0x4000` and
  `xip_map_base = 0`: the packer leaves about 9.5 KiB for the ROM boot image
  and the kernel ends 48 KiB earlier than the old 0x10000 layout.  A config
  can override the pair for a different variant.
- `[features]` turns on `-DTOS_<NAME>` for every source (e.g. `wifi` →
  `-DTOS_WIFI`, `net` → `-DTOS_NET`).  `false` leaves it undefined.
  A target can also declare default features: `rpi-pico` starts with
  `pwm`, `uart1`, `spi` and `i2c` on, so `tos build -target rpi-pico` needs no
  config; writing `pwm = false` (what menuconfig saves) compiles that driver
  and its user command out.
- Both numeric runtimes are always linked: `user/fix16.c` provides the
  explicit Q16.16 API (`fix16_*`, +/-32767.99998, step 1/65536, the small/fast
  path) and `user/softfloat32.c` backs ordinary `float` syntax with IEEE
  binary32.  Programs only pay for what they call: every function has its own
  section and the linker drops the rest, so an image that uses neither grows
  by nothing.
- `[features] softfloat-min = true` compiles that runtime with
  `TOS_SOFTFLOAT_MIN`: subnormals flush to zero and NaN/Inf are not
  represented (inputs clamp to the largest finite value, overflow and x/0
  saturate).  It saves another ~580 bytes; use plain soft float when the
  program depends on those IEEE corner cases.
- `[features] diag = true` compiles the crash/liveness record in
  (`kernel/diag.h`): per-core tick heartbeat, "inside a masked section / a
  spin lock / a flash park right now", the last masked-section site and
  caller, and a `[PREV]` line printed on the next boot from `.noinit` state
  that survived the reset.  Off by default and costing nothing when off.
  Use it when a board wedges silently; `addr2line -e build/kernel-<target>.elf`
  resolves the printed return addresses.
- `[drivers]` lists directories to add to the source/include paths.  Every
  `*.c`/`*.S` in that directory is compiled.  These are **in addition to** the
  target's board-default drivers (`Targets[..].Drivers`).  A special key
  `exclude = drivers/display/st7789` removes directories from the
  target's default list (GPIO, serial/USB console and SPI are required).
- `[wifi]` supplies the Espressif blob directory, the ROM symbol linker
  script and the symbol that keeps `wifi_probe` alive.
- `[user] exclude` drops named `user/*.c` programs or `user/<dir>` programs.
- `[sources] exclude` drops individual kernel/driver source paths.
- `[ld] extra` appends free-form linker flags.

Examples:

| file | purpose |
| --- | --- |
| `tos.conf` | default target (x86) |
| `configs/esp32c3.conf` | C3 kernel only, no WiFi/net commands |
| `configs/esp32c3-wifi.conf` | C3 + WiFi + DHCP/ICMP, all commands |
| `configs/esp32c3-wifi-softmin.conf` | the same, with the saturating float runtime |
| `configs/esp32c3-wifi-nonet.conf` | C3 + WiFi association only |
| `configs/esp32c3-wifi-ble.conf` | C3 + WiFi/net, `-Oz`; the BLE-headroom variant |
| `configs/rpi-pico.conf` | RP2040, WiFi commands excluded |
| `configs/rp2350.conf` | RP2350, WiFi commands excluded |
| `configs/atk-dnr2350am-rv32.conf` | RP2350 Hazard3, WiFi commands excluded |
| `configs/x86-threads.conf` | x86 with the cooperative thread library (`swcost`, `threadtest`) |
| `configs/c3-kthreads.conf` | ESP32-C3 with the experimental kernel-thread API (`threadbench`) |
| `configs/esp32c3-16k.conf` | ESP32-C3 without WiFi, 16 KiB heap and 16 task slots (measured peak 1.5 KiB; 12 background jobs) |
| `configs/arm-kthreads.conf` | QEMU mps2 (Cortex-M3) with kernel threads |
| `configs/atk-dnr2350am-smp-kthreads.conf` | RP2350 Cortex-M33 dual core with kernel threads |
| `configs/atk-dnr2350am-rv32-smp-kthreads.conf` | RP2350 Hazard3 RISC-V dual core with kernel threads |
| `configs/release.conf` | target-agnostic release build: boot traces and diagnostics compiled out (`boot_trace = 0`), everything else at the target's defaults |
