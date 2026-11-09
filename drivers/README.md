# Drivers

Drivers are no longer compiled unconditionally from `kernel/` or `board/`.
Each target declares its board-default driver directories in
`internal/build/build.go` (`Targets[..].Drivers`), so
`tos build -target <board>` keeps working without a config.  A build config's
`[drivers]` section **adds** to that list:

```ini
[drivers]
wifi = drivers/wifi/esp32c3
```

Every `*.c` / `*.S` in a listed directory is compiled, and the directory (plus
`<dir>/include` when it exists) is added to the include path.  Board pin maps,
clocks, memmap headers and linker scripts stay in `board/<board>/`; drivers
read them through `platform.h` / the board headers.

## Layout

```
drivers/
  gpio/
    esp32c3/            GPIO/ADC
    rp2040/             GPIO/ADC
    rp2350/             GPIO/ADC
    rp2350rv/           GPIO/ADC (Hazard3)
  serial/esp32c3/       USB-Serial/JTAG console
  spi/
    rp2350/             SPI + spi.h
    rp2350rv/
  display/st7789/
    rp2350/             ST7789 panel + font5x7.h
    rp2350rv/
  usb/
    rp2040/             direct USB CDC
    rp2350/             TinyUSB CDC + tusb_config.h + shim
    rp2350rv/
  wifi/esp32c3/         ESP32-C3 WiFi stack
    wifi_drv.c          /dev/wifi, scan, connect, STA config
    wifi_osi.c          Espressif OSI table, DMA aliases, timers, PHY/MAC
    wifi_crypto.c       ROM SHA/AES/PBKDF2/CMAC callbacks
    wifi_wpa.c          wpa_funcs table + WPA2-PSK 4-way handshake
    wifi_net.c          ARP/IPv4/ICMP, DHCP client+renew, DNS, TCP client
    wifi_shim.c         libc/soft-float/printf shims for the blobs
    wifi_phy_data.c     default C3 PHY init data
    lib/                Espressif prebuilt libs + esp32c3.rom.ld
```

## Adding a driver

1. Put it in `drivers/<class>/<name>/` with its headers beside the sources.
2. Add the directory to the target's `Drivers` list, or to a config's
   `[drivers]` section.
3. Gate optional code with `[features]` flags (`-DTOS_<NAME>`).
