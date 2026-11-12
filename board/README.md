# board/ — 板级目标索引

每个目录是一块板子（或一台虚拟机）的板级实现：`platform.c` 负责时钟/控制台/定时器/
flash，`layout.c` 给出内存与文件系统分区，`kernel.ld` 决定镜像落点。同一套内核和用户
程序在这些目标之间共用，换板子只换这几个文件。

每块板子的构建、烧写、控制台和移植进度都在各自的 README 里：

| 目录 | 目标 | 平台 | 怎么跑 | 状态 |
| --- | --- | --- | --- | --- |
| [`pc/`](pc/README.md) | `x86` | i386 / 传统 BIOS + MBR | QEMU 或写盘启动 | ✅ 完整 |
| [`qemu-virt/`](qemu-virt/README.md) | `rv32`、`rv32smp`、`rv32nc` | QEMU virt，RV32IM(C) | `tos run` | ✅ 完整（含 SMP） |
| [`qemu-mps2/`](qemu-mps2/README.md) | `arm` | QEMU mps2-an385，Cortex-M3 | `tos run` | ✅ 完整 |
| [`esp32c3/`](esp32c3/README.md) | `esp32c3` | ESP32-C3，XIP flash | esptool / `scripts/esp-c3/flash.py` | ✅ 实机（含 WiFi） |
| [`rp2040/`](rp2040/README.md) | `rp2040`、`rp2040smp` | Pico，Cortex-M0+ | BOOTSEL UF2 | ◐ UART 控制台可用，USB CDC 调试中 |
| [`rp2350/`](rp2350/README.md) | `rp2350`、`rp2350smp` | Pico 2 / ATK-DNRP2350AM，Cortex-M33 | BOOTSEL UF2 | ✅ 实机（USB + 屏 + MPU） |
| [`rp2350rv/`](rp2350rv/README.md) | `rp2350rv`、`rp2350rvsmp` | 同一块 RP2350 的 Hazard3 RISC-V | BOOTSEL UF2 | ✅ 实机（含双 hart） |

## 选哪个目标

- 只想跑一下 / 改内核：`x86`（默认）或 `rv32`，QEMU 里最快，`make run` 就是 x86。
- 验证 PMP：`rv32` / `esp32c3`；验证 ARMv7-M MPU：`arm`；验证 PMSAv8 MPU：`rp2350`。
- 验证"没有 MPU 的 MCU"：`rp2040`（任务之间没有硬件隔离）。
- 验证 SMP：`rv32smp`、`rp2350smp`、`rp2350rvsmp`。
- 真机：C3 走串口烧写；三块 RP 系列都是 BOOTSEL 拖 UF2。

## 公共约定

- 构建：`go run ./cmd/tos build -target <target>`；产物都在 `build/`。
- 配置：`tos.conf` 或 `-config configs/<name>.conf`；`tos menuconfig` 交互编辑。
- QEMU 目标：`go run ./cmd/tos run -target <target> [-headless] [-debug]`。
- 真机目标：**没有** `tos run`，用各自 README 里的烧写脚本。
- 每块板子的 `layout.c` 里的常量必须和 `internal/build/` 的打包器一致，构建期
  `checkLayout` 会核对；改内存布局时两边一起改。

## 新增一块板子

1. 建 `board/<name>/`，至少要有 `platform.c`、`layout.c`、`kernel.ld`
   （ARM/RV32 的公共部分在 `arch/arm`、`arch/rv32`）。
2. 在 `internal/build/build.go` 的 `Targets` 里加一项（arch、triple、heap、窗口粒度等）。
3. flash 备份的目标要在 `internal/build/uf2.go` / `espimage.go` 里加镜像打包器，并在
   `checkLayout` 里核对分区偏移。
4. 照本目录里现有 README 的结构补一份，写清楚**哪些是实机验证过的、哪些只是编译通过**。
