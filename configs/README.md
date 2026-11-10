# Build configuration

`tos build` reads `tos.conf` at the tree root by default.  Pass
`-config configs/<name>.conf` (or set `TOS_CONFIG`) to choose another file.
`-target` still overrides `[build] target`.

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
- `[features] softfloat = true` links the IEEE binary32 runtime
  (`user/softfloat32.c`), which `float` arithmetic and `printfloat` need.
  It is off by default: numeric code then uses the always-linked `fix16_*`
  API (`user/fix16.c`), the small/fast path (Q16.16, +/-32767.99998, step
  1/65536).  There is no automatic float-on-fixed-point emulation, so neither
  mode pays a per-operation conversion.
- `[features] softfloat-min = true` goes one step further: the same runtime
  compiled with `TOS_SOFTFLOAT_MIN`, which drops subnormals (flush to zero)
  and NaN/Inf (inputs clamp to the largest finite value, overflow and x/0
  saturate).  It saves another ~300 bytes; use plain `softfloat` when the
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
| `configs/esp32c3-wifi-soft.conf` | the same, plus the 32-bit soft-float runtime |
| `configs/esp32c3-wifi-softmin.conf` | soft float without NaN/Inf/subnormals |
| `configs/esp32c3-wifi-nonet.conf` | C3 + WiFi association only |
| `configs/rp2040.conf` | RP2040, WiFi commands excluded |
| `configs/rp2350.conf` | RP2350, WiFi commands excluded |
| `configs/rp2350rv.conf` | RP2350 Hazard3, WiFi commands excluded |
