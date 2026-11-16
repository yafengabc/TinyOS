# 预编译镜像（GitHub Actions）

`.github/workflows/images.yml` 在每次推送到 `main`、推送 `v*` 标签或手动触发时，
构建可以直接刷进板子的镜像。

## 产物

| 目标 | 文件 | 刷写方式 |
| --- | --- | --- |
| `esp32c3` | `esp32c3-full.bin`（推荐，一次写完）+ `esp32c3.bin` / `ramdisk-esp32c3.bin`（分体） | `esptool write-flash 0x0 esp32c3-full.bin`；分体见下 |
| `rp2040` | `rp2040.uf2` | BOOTSEL 后拖到 `RPI-RP2` |
| `rp2350` | `rp2350.uf2` | BOOTSEL 后拖到 `RP2350` |
| `rp2350smp` | `rp2350smp.uf2` | 同上（双核） |
| `rp2350rv` | `rp2350rv.uf2` | 同上（RISC-V 核） |
| `rp2350rvsmp` | `rp2350rvsmp.uf2` | 同上（RISC-V 双核） |

每个目标还会带一个 `FLASH-<target>.txt`，里面是简短的刷写说明，以及一份
**`build-config.txt`**：这次构建用的 `heap` / `max_procs` / `kstack` / `opt` /
features 与"是否读了 `tos.conf`"。

最后一份是这次新加的，因为**发布镜像和上板验收用的构建可能不是同一份**：CI 没有
`tos.conf`（走 target 默认值），而本机验收通常有一份（`heap=12288`、`opt=-Oz` 之类）。
两者都能跑，但"验收过"这个结论不能直接搬到发布物上——现在产物里写明了它是按什么
构建的，比对一眼就够。

`build-config.txt` 里还有三行 `check-grain` / `check-stack` / `check-guard`：那是
**构建期 `TOS_WINDOW()` 校验**用的几何，不是运行时几何（运行时看
`board/*/layout.c`，两者的差异见 OPEN-ITEMS §36）。

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

**一次写完**（内核 + 文件系统在同一个文件里，和 MicroPython 的发布形态一样）：

```bash
esptool --chip esp32c3 write-flash 0x0 esp32c3-full.bin
```

**分体**（保留：只换内核时可以不动文件系统）：

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

- 运行环境：`ubuntu-24.04` + **`apt.llvm.org` 上最新的 clang/ld.lld**（优先 23，往下
  到 19）+ Go stable。**编译器版本是有意义的**：`user/msh/msh.c` 在 clang 23 的 `-Oz`
  下是 8132 B（离 8 KiB 预算 60 B），而发行版自带的 clang 18 编出来是 12 KiB——
  `TOS_ONE_GRAIN()` 会把它从 16 KiB 窗口推到 32 KiB，`rp2040`/`rp2040smp` 直接构建
  失败。镜像要是"硬件上量过的那一份"，就得用同一档编译器；装不上时 workflow 会
  打 `::warning::` 并退回流发行版 clang，失败点会落在体积检查上。
- 每个 job 结束前会打印 `wc -c build/user/bin/msh`，编译器漂移在日志里就看得见。
- 每个目标在独立的 matrix job 里执行 `go run ./cmd/tos build -target <target>`。
- 另有 **`qemu smoke` job**：构建 `x86`/`rv32`/`rv32smp`/`arm` 四个 QEMU 目标，
  各自跑 `tostest smoke`（`ps`、管道、`piperace`、`prot stack`、`deep 3`），再跑
  `scripts/soak.py --cycles 3`。**打 tag 时 `release` 依赖它**——内核起不来的提交
  不会发出镜像。
- `mklfs` 是构建期 host 工具，由同一个 clang 编译。
- 官方镜像里的用户程序使用 clang 构建；`goc32` 是独立的用户态 C 编译器实验，
  不在这个默认构建链路里。
- RP2040 的 littlefs 分区是 300 KiB。如果一个新增程序把文件系统撑爆，
  mklfs 会报 `No more free space`；这时需要精简该目标的用户程序或扩大分区
  配置，而不是忽略 CI 失败。
