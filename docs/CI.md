# 预编译镜像（GitHub Actions）

`.github/workflows/images.yml` 在每次推送到 `main`、推送 `v*` 标签或手动触发时，
构建可以直接刷进板子的镜像。

## 产物

| 目标 | 文件 | 刷写方式 |
| --- | --- | --- |
| `esp32c3` | `esp32c3.bin` + `ramdisk-esp32c3.bin` | `esptool` 分别写 `0x0` 和 `0x80000` |
| `rp2040` | `rp2040.uf2` | BOOTSEL 后拖到 `RPI-RP2` |
| `rp2350` | `rp2350.uf2` | BOOTSEL 后拖到 `RP2350` |
| `rp2350smp` | `rp2350smp.uf2` | 同上（双核） |
| `rp2350rv` | `rp2350rv.uf2` | 同上（RISC-V 核） |
| `rp2350rvsmp` | `rp2350rvsmp.uf2` | 同上（RISC-V 双核） |

每个目标还会带一个 `FLASH-<target>.txt`，里面是简短的刷写说明。

## 从哪里下载

### 普通构建

打开仓库的 **Actions → Build flashable images → 对应的一次运行**，页面底部
的 **Artifacts** 里有：

```text
tinyos-esp32c3
tinyos-rp2040
tinyos-rp2350
tinyos-rp2350smp
tinyos-rp2350rv
tinyos-rp2350rvsmp
```

Artifacts 默认保留 30 天。

### 标签发布

推送一个 `v*` 标签（例如 `v0.1.0`）时，所有镜像会自动附加到对应的
GitHub Release 上，长期保存：

```bash
git tag v0.1.0
git push origin v0.1.0
```

## ESP32-C3 刷写

```bash
esptool.py --chip esp32c3 write-flash 0x0     esp32c3.bin
esptool.py --chip esp32c3 write-flash 0x80000 ramdisk-esp32c3.bin
```

或新语法一次写完：

```bash
esptool --chip esp32c3 write-flash \
  0x0 esp32c3.bin \
  0x80000 ramdisk-esp32c3.bin
```

刷之前让板子进入下载模式（按住 BOOT，点一下 RESET）。

## RP2040 / RP2350 刷写

按住 BOOTSEL 插入 USB，电脑上会出现 `RPI-RP2`（RP2040）或 `RP2350`
（RP2350）U 盘，把对应的 `.uf2` 拖进去即可。复制完成后板子自动重启进入
TinyOS。

## 构建细节

- 运行环境：`ubuntu-24.04` + `clang` + `ld.lld` + Go stable。
- 每个目标在独立的 matrix job 里执行 `go run ./cmd/tos build -target <target>`。
- `mklfs` 是构建期 host 工具，由同一个 clang 编译。
- 官方镜像里的用户程序使用 clang 构建；`goc32` 是独立的用户态 C 编译器实验，
  不在这个默认构建链路里。
- RP2040 的 littlefs 分区是 300 KiB。如果一个新增程序把文件系统撑爆，
  mklfs 会报 `No more free space`；这时需要精简该目标的用户程序或扩大分区
  配置，而不是忽略 CI 失败。
