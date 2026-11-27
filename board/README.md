# board/ — 板级目标索引

每个目录是一块板子（或一台虚拟机）的板级实现：`platform.c` 负责时钟/控制台/定时器/
flash，`layout.c` 给出内存与文件系统分区，`kernel.ld` 决定镜像落点。同一套内核和用户
程序在这些目标之间共用，换板子只换这几个文件。

每个板级目录都必须有 `README.md`：没有的那块板等于没交付——别人不知道它烧什么、怎么连、
哪些是实机验过的。缺 README 的目录按未完成处理，README 里要写清楚**哪些是实机验证过的、
哪些只是编译通过**。构建、烧写、控制台和移植进度都在各自的 README 里：

| 目录 | 目标 | 平台 | 怎么跑 | 状态 |
| --- | --- | --- | --- | --- |
| [`pc/`](pc/README.md) | `x86` | i386 / 传统 BIOS + MBR | QEMU 或写盘启动 | ✅ 完整 |
| [`qemu-virt/`](qemu-virt/README.md) | `rv32`、`rv32smp`、`rv32nc` | QEMU virt，RV32IM(C) | `tos run` | ✅ 完整（含 SMP） |
| [`qemu-mps2/`](qemu-mps2/README.md) | `arm` | QEMU mps2-an385，Cortex-M3 | `tos run` | ✅ 完整 |
| [`esp32c3-generic/`](esp32c3-generic/README.md) | `esp32c3-generic` | ESP32-C3，XIP flash | esptool / `scripts/esp-c3/flash.py` | ✅ 实机（含 WiFi）；I2C/SPI/PWM 命令级实机通过，波形/从设备待测（OPEN-ITEMS §11） |
| [`rpi-pico/`](rpi-pico/README.md) | `rpi-pico`、`rpi-pico-smp` | Pico，Cortex-M0+ | BOOTSEL UF2 | ✅ 实机（UART0 + USB CDC；无 MPU） |
| [`rp2350-generic/`](rp2350-generic/README.md) | `rp2350-generic` | RP2350 芯片基线，Cortex-M33，无面板/无卡槽 | BOOTSEL UF2 | ◐ 芯片基线：控制台/MPU/RAM disk；无独立验收卡 |
| [`atk-dnr2350am/`](atk-dnr2350am/README.md) | `atk-dnr2350am`、`atk-dnr2350am-smp` | ATK-DNRP2350AM，Cortex-M33 | BOOTSEL UF2 | ✅ 实机（USB + 屏 + MPU） |
| [`rpi-pico2/`](rpi-pico2/README.md) | `rpi-pico2`、`rpi-pico2-smp` | Raspberry Pi Pico 2，Cortex-M33 | BOOTSEL UF2 | ✅ 实机（四镜像回归，MPU + 双核） |
| [`rpi-pico2-rv32/`](rpi-pico2-rv32/README.md) | `rpi-pico2-rv32`、`rpi-pico2-rv32-smp` | 同一块 Pico 2 的 Hazard3 | BOOTSEL UF2 | ✅ 实机（PMP + 双 hart） |
| [`rp2350rv-generic/`](rp2350rv-generic/README.md) | `rp2350rv-generic` | 同一颗 RP2350 的 Hazard3 芯片基线 | BOOTSEL UF2 | ◐ 芯片基线：RISC-V 控制台 + PMP + RAM disk |
| [`atk-dnr2350am-rv32/`](atk-dnr2350am-rv32/README.md) | `atk-dnr2350am-rv32`、`atk-dnr2350am-rv32-smp` | 同一块 RP2350 的 Hazard3 RISC-V | BOOTSEL UF2 | ✅ 实机（含双 hart） |

## 选哪个目标

- 只想跑一下 / 改内核：`x86`（默认）或 `rv32`，QEMU 里最快，`make run` 就是 x86。
- 验证 PMP：`rv32` / `esp32c3-generic`；验证 ARMv7-M MPU：`arm`；验证 PMSAv8 MPU：`atk-dnr2350am`。
- 验证"没有 MPU 的 MCU"：`rpi-pico`（任务之间没有硬件隔离）。
- 验证 SMP：`rv32smp`、`atk-dnr2350am-smp`、`atk-dnr2350am-rv32-smp`。
- 真机：C3 走串口烧写；三块 RP 系列都是 BOOTSEL 拖 UF2。

## 公共约定

- 构建：`go run ./cmd/tos build -target <target>`；产物都在 `build/`。`go run ./cmd/tos build all`
  一次构建 `targets/*.conf` 里的全部目标（不应用 `-config`）。
- 配置：`tos.conf` 或 `-config configs/<name>.conf`；`tos menuconfig` 交互编辑。
- QEMU 目标：`go run ./cmd/tos run -target <target> [-headless] [-debug]`。
- 真机目标：**没有** `tos run`，用各自 README 里的烧写脚本。
- 每块板子的 `layout.c` 里的常量必须和 `internal/build/` 的打包器一致，构建期
  `checkLayout` 会核对；改内存布局时两边一起改。

## 新增一块板子

1. 建 `board/<name>/`，至少要有 `platform.c`、`layout.c`、`kernel.ld`
   （ARM/RV32 的公共部分在 `arch/arm`、`arch/rv32`）。只是同一颗芯片的另一块板，
   用 `base =` 继承芯片基线，只写 `board.conf` + 设备表就够了（见 `rpi-pico2/`）。
2. 在 `targets/<name>.conf` 里加一项（`board`、`arch`、`heap` 等镜像旋钮），
   板级事实写进 `board/<name>/board.conf`；`internal/build/targets.go` 负责加载。
3. flash 备份的目标要在 `internal/build/uf2.go` / `espimage.go` 里加镜像打包器，并在
   `checkLayout` 里核对分区偏移。
4. **必须**照本目录里现有 README 的结构补一份 `board/<name>/README.md`，写清楚
   **哪些是实机验证过的、哪些只是编译通过**，并回填上面表格里的那一行。
