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
- `[features]` turns on `-DTOS_<NAME>` for every source (e.g. `wifi` →
  `-DTOS_WIFI`, `net` → `-DTOS_NET`).  `false` leaves it undefined.
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
  target's board-default drivers (`Targets[..].Drivers`).
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
