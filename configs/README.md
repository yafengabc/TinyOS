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
target = esp32c3

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
- `[features]` turns on `-DTOS_<NAME>` for every source (e.g. `wifi` →
  `-DTOS_WIFI`, `net` → `-DTOS_NET`).  `false` leaves it undefined.
  A target can also declare default features: `rp2040` starts with
  `pwm`, `uart1`, `spi` and `i2c` on, so `tos build -target rp2040` needs no
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
- `[drivers]` lists directories to add to the source/include paths.  Every
  `*.c`/`*.S` in that directory is compiled.  These are **in addition to** the
  target's board-default drivers (`Targets[..].Drivers`).  A special key
  `exclude = drivers/display/st7789/rp2350` removes directories from the
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
| `configs/rp2040.conf` | RP2040, WiFi commands excluded |
| `configs/rp2350.conf` | RP2350, WiFi commands excluded |
| `configs/rp2350rv.conf` | RP2350 Hazard3, WiFi commands excluded |
| `configs/x86-threads.conf` | x86 with the cooperative thread library (`swcost`, `threadtest`) |
| `configs/c3-kthreads.conf` | ESP32-C3 with the experimental kernel-thread API (`threadbench`) |
| `configs/arm-kthreads.conf` | QEMU mps2 (Cortex-M3) with kernel threads |
| `configs/rp2350smp-kthreads.conf` | RP2350 Cortex-M33 dual core with kernel threads |
| `configs/rp2350rvsmp-kthreads.conf` | RP2350 Hazard3 RISC-V dual core with kernel threads |
