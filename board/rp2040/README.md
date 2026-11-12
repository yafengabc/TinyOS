# Raspberry Pi Pico / RP2040（`-target rp2040` / `rp2040smp`）

Cortex-M0+（ARMv6-M）真机目标。内核 XIP 在 flash 里原地跑，文件系统在 QSPI flash
上，控制台目前以 UART0 为主、USB CDC 同步在接。

## 硬件与启动链

| 项 | 值 |
| --- | --- |
| CPU | 双 Cortex-M0+，125 MHz（PLL_SYS）；`rp2040smp` 才拉起 core 1 |
| SRAM | 264 KiB @ `0x20000000`，内核栈在顶部 |
| Flash | 2 MiB QSPI，XIP 映射在 `0x10000000` |
| 启动 | ROM 把 256 B stage-2（`board/rp2040/boot2.bin`）搬进 SRAM 并校验 CRC-32/MPEG-2；stage-2 配好 XIP 后跳到 `0x10000100` 取 MSP/reset；VTOR 由我们自己设 |
| UF2 | family `0xe48bff56`，产物 `build/rp2040.uf2` |
| 文件系统 | littlefs @ flash `0x80000`，1.5 MiB（384 块）；随镜像发布 300 KiB，首挂载拉伸 |
| 任务窗口 | 16 KiB 粒度，默认栈 8 KiB、栈下护栏 4 KiB |
| 内核堆 / 进程表 / 内核栈 | 16 KiB / 8 槽 / 8 KiB（`configs/rp2040.conf` 可改） |
| 内存保护 | **没有**（M0+ 无 MPU/MMU），只有非特权态，`prot` 默认不打包 |

时钟：ROM 交接时 clk_sys 在 ROSC 上（~5 MHz），`clocks_init()` 打开 XOSC、把
clk_ref 切到晶振、再把 clk_sys 切到 PLL_SYS 125 MHz；UART 分频、SysTick、PLL_USB
的 12 MHz 参考都依赖这一步。

## 构建

```bash
go run ./cmd/tos build -target rp2040
# -> build/kernel-rp2040.elf
# -> build/ramdisk-rp2040.bin
# -> build/rp2040.uf2

go run ./cmd/tos build -target rp2040smp   # 双核，其余相同
```

默认打开 `pwm/uart1/spi/i2c/dma` 五个外设特性；用 menuconfig 或配置文件关掉任意
一个，驱动代码和对应命令一起不编译：

```bash
go run ./cmd/tos menuconfig -config configs/rp2040.conf
```

`configs/rp2040.conf` 额外排除了 WiFi/网络命令和 `prot`。

## 烧写与看日志

1. 拔掉 USB，按住 **BOOTSEL** 再插上（或按住 BOOTSEL 点 RESET），出现 `RPI-RP2` 盘。
2. 把 `build/rp2040.uf2` 拖到盘上，板子自动重启。

一块板反复烧的时候用 watcher，按一次键完成"烧写 + 打开串口 + 抓启动日志"：

```bash
python scripts/rp2040/autoflash.py --uf2 build/rp2040.uf2          # 一次性
python scripts/rp2040/autoflash.py --uf2 build/rp2040.uf2 --watch  # 常驻，之后每次按键都烧
python scripts/rp2040/autoflash.py --uf2 a.uf2 --uf2 b.uf2 --watch # 队列：每次烧下一张
```

脚本会在拷包**之前**打开串口，所以不会丢启动字符；每轮日志写到
`build/rp2040-autoflash/`。`--port` 默认 `COM19`，按自己适配器的实际端口传
（`--port COM8`）。这块板的 CH343 的 DTR/RTS 没有接 BOOTSEL/RESET，
所以进 BOOTSEL 只能按按钮。

已经跑起来之后，还有一个软件回退通道：控制台上 **Ctrl-D**（0x04）通过 ROM 的
`ROM_TABLE_CODE('U','B')` 重新进 BOOTSEL，不用再按按钮。同一组魔术键还有
Ctrl-R（重新枚举 USB）、Ctrl-T（打印 USB 中断 trace）、Ctrl-E/Ctrl-W（E15
workaround 开/关）、Ctrl-B（直接测 ROM 调用）。

## 控制台

- **UART0（GP0=TX / GP1=RX，115200 8N1）是当前主控制台**，启动探针、内核日志、
  shell 都从这里出。
- **USB CDC** 走 `drivers/usb/rp2040/` 的 TinyUSB 设备栈，初始化后镜像到同一条
  控制台流；枚举问题仍在调试（见下）。
- 板载 LED 是 GP25，`echo 1 > /dev/led0` 可点灯。

## 外设与用户命令

`board/rp2040/board_devs.c` 是这块板的"设备树"（30 GPIO、UART0/1、SPI0/1、
I2C0/1、ADC0..3 + 片内温度 ADC4、16 通道 PWM、两个 PIO、12 通道 DMA、USB 1.1、
2 MiB QSPI）。`cat /proc/dev` 看整张表。

```sh
pwm 25 1000 50     # GP25：1 kHz、50% 占空比（板载 LED 变暗）
pwm 25 off         # 停止 PWM，引脚交回 SIO
uart               # UART1：TX=GP4、RX=GP5、115200
uart send HELLO
uart recv 500
spi / i2c / dma    # SPI0/1、I2C0/1、DMA 的用户命令
pio asm /pio/loopback.pio 0 0   # 汇编并在 PIO0 SM0 装入 PIO 程序
pio start 0 0; pio tx 0 0 1 0 1 1; pio rx 0 0 4
pio asm /pio/blink.pio 0 0; pio start 0 0   # GP25 约 2 Hz
```

PIO 的汇编器在用户态（`user/pio.c`），指令存储只有 32 个 16 位字，所以可以
在控制台里改一行 `.pio`、`pio asm`、看示波器，不用重烧。完整的语法、`SYS_PIO`
ABI、生命周期（进程退出自动停 SM 并交回引脚；重启会丢失，需要重新装载）见
`docs/PIO.md`。

## 移植进度

保守口径：✅ = 真机确认过；◐ = 代码已实现、构建通过，但还没有完整验收；
☐ = 未做。

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| boot2 + UF2 + XIP 启动 | ✅ | ROM CRC、family、向量位、MSP 都在构建期校验 |
| 125 MHz 时钟、SysTick、UART0 分频 | ✅ | 启动探针逐段打印，卡住能定位 |
| UART0 控制台 + shell | ✅ 实机 | 启动探针、提示符都从 GP0/GP1 出；`scripts/rp2040/autoflash.py --probe/--accept` 就是按"能到 shell"写的 |
| littlefs @ flash 0x80000（ROM flash API） | ✅ 挂载 / ◐ 写 | shell 能启动说明根文件系统挂上了；写/擦路径要用 `dd` 再验 |
| USB CDC（TinyUSB，IRQ 5） | ◐ 调试中 | 目前 trace 是 BUS_RESET + DEV_SUSPEND、没有 SETUP_REQ；已按 erratum RP2040-E15 做 forced-J，Ctrl-E/W 可 A/B |
| PWM / UART1 / SPI0-1 / I2C0-1 / DMA | ◐ | 驱动 + 用户命令已接，menuconfig 可裁；实机覆盖以自测为准 |
| PIO（2 块 / 8 个 SM + 用户态汇编器） | ◐ | HAL + `SYS_PIO` + `pio asm/start/tx/rx/rel`；汇编器已在 x86 上核对编码，Pico 实机待测（见 `docs/PIO.md`） |
| `rp2040smp`（core 1） | ◐ | 目标可构建，握手走 SIO FIFO；未做完整验收 |
| BOOTSEL 回退（Ctrl-D） | ◐ | ROM 调用路径已实现，含看门狗 scratch 标记 |
| 内存保护 | ☐ | 硬件没有，`prot` 在本板会真的写穿；默认不打包 |
| `reboot`（AIRCR.SYSRESETREQ） | ◐ | 代码在，注释里仍标"未实测" |

### 排障：先确认"我们的 UF2 能不能跑"

`scripts/rp2040-blink/` 是一个 432 字节的独立闪灯固件（不碰 USB/UART/littlefs），
复用一份已知能启动的 boot2 打包，用来把"镜像/启动层"和"TinyOS 代码"分开：

```bash
python scripts/rp2040-blink/build.py \
    --boot2-from build/micropython-rpi_pico.uf2 -o build/rp2040-blink.uf2
```

烧进去看 GP0：闪 → boot2/向量/链接都对，问题在 OS 里；不闪 → 先查镜像层。
注意 `build/` 是被 git 忽略的，另一台机器上要自带一份能启动的 RP2040 UF2。

## 已知限制

- 没有 MPU/MMU：任务之间没有硬件隔离（和 x86 同级的"只隔离特权级"）。
- 只有 264 KiB SRAM，`sh`/`edit` 这类 32 KiB 窗口一次能放十几个，`py` 放不下。
- USB 控制台还没跑通，接 USB 只有供电作用；串口适配器是必需品。
- 内核与 littlefs 共享 2 MiB flash：内核在 `0x10000..0x80000`，之后 1.5 MiB 给文件系统。

## 相关文件

- `board/rp2040/{platform.c,layout.c,kernel.ld,arm_memmap.h,boot2.bin,board_devs.c}`
- `board/rp2040/{uart_dbg.c,uart1.c,spi.c,i2c.c,dma.c}`：外设与调试串口
- `drivers/usb/rp2040/`：TinyUSB 设备栈与 CDC-ACM
- `drivers/gpio/rp2040/`：GPIO/ADC
- `internal/build/uf2.go`：`emitRP2040UF2()`、boot2 CRC、布局校验
- `scripts/rp2040/autoflash.py`、`scripts/rp2040-blink/`
