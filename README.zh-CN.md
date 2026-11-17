# TinyOS

[![Build flashable images](https://github.com/yafengabc/TinyOS/actions/workflows/images.yml/badge.svg)](https://github.com/yafengabc/TinyOS/actions/workflows/images.yml)
[![Latest release](https://img.shields.io/github/v/release/yafengabc/TinyOS)](https://github.com/yafengabc/TinyOS/releases)

一个用 **C + 汇编** 写的极简 32 位抢占式操作系统：**一套内核、一套用户态**，覆盖
**12 个构建目标**——从 264 KiB RAM 的 Cortex-M0+ 到 QEMU 和 PC。程序跑在**用户态**
（x86 ring 3 / RISC-V U-mode / ARM Thread+PSP），各自待在自己的窗口里，**崩一个程序
只杀它自己**。

它不是 Linux 的克隆。它是一套小而完整、能从头读到尾的系统：调度器、无 MMU 的内存
保护、文件系统、shell、驱动，以及用 Go 写的构建与测试工具链。

## 为什么不一样

- **不需要 SDK**。12 个目标——M0+、M33、Hazard3、RV32、i386——全都由**同一个
  `clang` + `ld.lld`** 构建，别的不需要。没有 pico-sdk 安装、没有 ESP-IDF 安装、
  没有厂商构建系统：boot2 桩、ROM 函数表、pad 寄存器、TinyUSB 要的那层 pico-sdk
  形状的门面，都 vendored 在 `third_party/` 和 `board/` 里。**加一块板子是加一个
  目录，不是装一套 SDK。**
- **编译器就在仓库里**。`goc32`（子模块）一条命令把 C 编成 `tos build` 会放进
  littlefs 的那个 **LZ4TOS** 封装——窗口提示、栈/堆申请、LZ4 数据全在里面——覆盖
  RV32IMC、Thumb-1/ARMv6-M、ARMv7-M、ARMv8-M。不用 libLLVM、不用 cgo，**也不需
  要外部工具链**：汇编器和链接器就在 `internal/as` 和 `internal/ld`。产出的
  `.tos` 丢进 `build/fs-stage/bin/`，重建 ramdisk 就装进去了。（goc32 的目标码
  比 clang 大；这没关系——发布镜像和预编译的 `msh` 继续用 clang，goc32 负责
  快速单文件开发。）
- **一套内核、一套用户态、12 个目标**——包括同一颗 RP2350 硅片上的两种指令集，
  以及一台 PC。
- **小到能读完**——内核约 70 KiB flash、约 45 KiB RAM，整个系统是一个下午能从头
  读到尾的仓库。

树里唯一的预编译二进制是 256 字节的 RP2040 boot2，以及**可选**的 ESP32-C3 WiFi
blob。

## 一览

| 目标 | CPU | 板子 | RAM | Flash | 状态 |
| --- | --- | --- | --- | --- | --- |
| `rpi-pico`, `rpi-pico-smp` | 2× Cortex-M0+ | Raspberry Pi Pico | 264 KiB | 2 MiB | 真机验过 |
| `atk-dnr2350am`, `atk-dnr2350am-smp` | 2× Cortex-M33 | Pico 2 / ATK-DNRP2350AM | 520 KiB | 2–4 MiB | 真机验过 |
| `atk-dnr2350am-rv32`, `atk-dnr2350am-rv32-smp` | 2× Hazard3（RISC-V） | 同一块板的另一个 ISA | 520 KiB | 2–4 MiB | 真机验过 |
| `esp32c3-generic` | RV32IMC | ESP32-C3 开发板 | 400 KiB | 4 MiB | 真机验过 |
| `rv32`, `rv32smp`, `rv32nc` | RV32IMC(+A) | QEMU virt | 256 KiB 堆 | — | CI 冒烟 + soak |
| `arm` | Cortex-M3 | QEMU mps2-an385 | — | — | CI 冒烟 |
| `x86` | i386 | PC / QEMU | — | 磁盘镜像 | 仅本机（见 OPEN-ITEMS） |

带 `smp` 的目标双核都跑；同板的单核版本是基线，用来区分"SMP 特有"和"普通"的 bug。

## 亮点

- **抢占式调度**——三级优先级、`sleep`/`prio`/`nice`、每任务 CPU 记账，三块板上
  有 **SMP**（RP2040、RP2350 M33、RP2350 Hazard3）：每核 run queue、IPI 式唤醒、
  flash 停核握手、每核 SCB/MPU 状态。
- **无 MMU 的用户态隔离**——每个任务一个窗口（镜像 + 栈 + 护栏洞），由 PMSAv8
  （M33）、PMP TOR（C3）、PMP NAPOT（Hazard3）或软件栈哨兵（M0+、x86）保证。
  粒度 4 KiB：小工具 16 KiB，`msh` 也是 16 KiB。
- **一套真用户态**——从 littlefs 加载的 PIE ELF：`msh` 加带管道/重定向的 `sh`，
  40 多个工具（`ls dd df free ps cat wc edit gpio adc pwm spi i2c dma pio crash deep
  piperace`），一个 Python 解释器（`py`），以及线程、管道、邮箱、信号的示例。
- **能活下来的文件系统**——flash / ATA / 内嵌三种后端上的 littlefs 2.x，驱动前有
  块缓存；C3 上还有**一体刷机镜像**（`esptool write-flash 0x0` 一次写完）。
- **接口**——devfs（`/dev/gpio4`、`/dev/lcd0`…）配 `ioctl` 设备模型、procfs
  （`/proc/ps`、`/proc/mem`…）、管道、信号、消息邮箱。
- **驱动**——UART（16550/CMSDK/PL011/C3/USJ）、GPIO/ADC/PWM/SPI/I2C/DMA、四个 RP
  目标上的 PIO、ST7789 屏、USB CDC（RP 用 TinyUSB，C3 用原生 USB）、C3 的 WiFi。
- **CI 既构建也运行**——每次推送出七个可刷写镜像，LLVM 固定到量尺寸时的版本，
  另有 QEMU 冒烟 + soak job，tag 自动发 Release。每个产物都带 `build-config.txt`，
  写明它是按什么配置构建的。

## 快速开始

需要 **Go**（1.22+）与 `PATH` 上的 **clang** / **ld.lld**——**别的不需要**。没有
SDK 要装：板子需要的一切都在仓库里。

```sh
# 构建任意目标
go run ./cmd/tos build -target atk-dnr2350am        # -> build/tinyos-atk-dnr2350am.uf2
go run ./cmd/tos build -target esp32c3-generic       # -> build/tinyos-esp32c3-generic.bin + -full.bin
go run ./cmd/tos build all                   # 一次构建 targets/*.conf 里的全部 15 个镜像

# RP2040 / RP2350：按住 BOOTSEL 插 USB，把 .uf2 拷进 U 盘
cp build/tinyos-atk-dnr2350am.uf2 /media/$USER/RP2350/

# ESP32-C3：一次写完（内核 + 文件系统在同一个文件里）
python scripts/esp-c3/flash.py --full -p /dev/ttyACM0

# QEMU：启动并驱动 shell
go run ./cmd/tostest smoke -target rv32
```

起来的是 `msh`，可以试 `ls`、`cat readme.txt`、`ps`、`free`、`echo hi > /f`、
`cat /f | wc`、`deep 3`、`prot stack`、`py demo.py`。

## 快速循环（不重刷镜像）

程序就是 littlefs 里的一个文件，所以改一行代码不需要重建 300 KiB 的文件系统镜像，
更不需要重新刷机：

```sh
goc32 user/foo.c -target atk-dnr2350am -o build/foo.tos   # C -> LZ4TOS blob
tosput-cli COM14 build/foo.tos /bin/foo            # base64 + CRC32，走控制台
tos> foo
```

`tosput-cli` 和板上的 `rx` 说同一套行协议（raw 模式、停等流控、对**落盘字节**算
CRC32），所以任何有控制台的板子都能用，内核侧只需要 `ttyraw()`。帧格式以及"为什么
没有硬件流控的 UART 上停等是必须的"见
[`docs/FILE-TRANSFER.md`](docs/FILE-TRANSFER.md)。

上一次 `tos build` 的功能宏（`TOS_THREADS`、`TOS_MAILBOX`、`TOS_NET` 等）会
自动复用；构建配置打开线程/kthread 时，运行时也会一起链接。clang 生成的目标码仍比
`goc32` 小一些，所以发布镜像继续用 clang；上面这个循环是给"十秒钟一轮"的开发用的。

## 测试

```sh
go run ./cmd/tostest smoke -target rv32     # QEMU：启动 + 命令脚本
python scripts/esp-c3/accept.py -p COM4     # ESP32-C3：24 项验收
python scripts/rp2350/accept33.py --image atk-dnr2350am-rv32-smp --uf2 build/tinyos-atk-dnr2350am-rv32-smp.uf2
python scripts/soak.py --cycles 8           # exec/堆/段抖动，free 不许漂移
```

真机结论都在 [`docs/HISTORY.md`](docs/HISTORY.md)：ESP32-C3 24/24；RP2350（M33）
SMP 16/16、`smpbench` 1.95x；RP2350（Hazard3）SMP 17/17、2.00x；两个 RP2350 镜像
PIO 17/17；RP2040 1000 轮 `dd`+`rm`。

## 文档

| 文档 | 内容 |
| --- | --- |
| [`docs/GUIDE.zh-CN.md`](docs/GUIDE.zh-CN.md) | 长手册：逐板用法、内存布局、系统调用 ABI |
| [`docs/TESTING.md`](docs/TESTING.md) | 回归测试手册：分级、命令、证据模板（所有 agent 必读） |
| [`AGENTS.md`](AGENTS.md) | agent 规范：测试、安全、硬件 |
| [`docs/PLATFORM-DESIGN.md`](docs/PLATFORM-DESIGN.md) | 移植契约、trap/用户态入口、PMP/MPU 布局 |
| [`docs/REAL-THREADS.md`](docs/REAL-THREADS.md) | kthreads 的设计与 SMP 规则 |
| [`docs/SHELL-GUIDE.md`](docs/SHELL-GUIDE.md) | shell、工具与用户态 API |
| [`docs/PIO.md`](docs/PIO.md) | PIO 装载器与控制台用法 |
| [`docs/CI.md`](docs/CI.md) | 镜像、发布、QEMU 门禁、工具链固定 |
| [`docs/OPEN-ITEMS.md`](docs/OPEN-ITEMS.md) | **现在**还开着什么 |
| [`docs/HISTORY.md`](docs/HISTORY.md) | 所有已关闭的条目，以及关掉它的那次实测 |
| [`docs/NUTTX-COMPARE.md`](docs/NUTTX-COMPARE.md) | 与 NuttX/Zephyr 的对照笔记 |

## 已知限制

- **x86 没有分页**——只有特权级隔离，没有每任务内存保护（和 M0+ 打开软件守卫之前
  一样）；
- **Cortex-M33 的 `deep 6`** 仍会 fault：异常入口需要在栈下面有一块可写着陆区，而
  护栏洞只留了 2.5 KiB；
- **RP2040 的 USB CDC** 还在调试，串口是进系统的路；
- 任何地方都没有 MMU："保护"指每任务一个窗口，不是虚拟内存。

## 仓库结构

```
kernel/     调度、进程、tty/控制台、littlefs 胶水、devfs、procfs
arch/       x86 / rv32 / arm 端口（trap 入口、用户态、PMP/MPU）
board/      每块板一个目录：时钟、控制台、布局、flash
drivers/    uart、gpio、adc、pwm、spi、i2c、dma、pio、usb、wifi、显示
user/       用户态：msh、sh、40+ 工具、libc、一个 Python 解释器
cmd/        tos（构建/烧写/打包）与 tostest（QEMU 测试台）
internal/   构建系统、镜像打包、文件系统镜像生成
docs/       设计、指南，以及 open/history 两份日志
```

## 相关项目

- [goc32](https://github.com/yafengabc/goc32) —— 用户态 C 编译器实验（RV32/Thumb），
  不在默认构建链路里。
- [RV32EMU](https://github.com/yafengabc/RV32EMU) 与
  [RV32EMU-C](https://github.com/yafengabc/RV32EMU-C) —— Go 与 C 两个 ESP32-C3
  模拟器，都能启动 TinyOS 镜像。
