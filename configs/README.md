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
| `configs/esp32c3-wifi-nonet.conf` | C3 + WiFi association only |
| `configs/rp2040.conf` | RP2040, WiFi commands excluded |
| `configs/rp2350.conf` | RP2350, WiFi commands excluded |
| `configs/rp2350rv.conf` | RP2350 Hazard3, WiFi commands excluded |
