# RP2350 芯片基线 / Cortex-M33（`-target rp2350-generic`）

RP2350 的"最小镜像"：**只声明硅片**（UART0/USB、GPIO/SPI/I2C/PWM/DMA/PIO），
不声明任何板级引脚，也不声明 flash 容量。文件系统是链接进内核的 RAM disk，
所以配 1 MiB、4 MiB、8 MiB 的 flash 或者根本没焊 flash 的板子都能跑同一个镜像。

具体板子在这一层上加 `base = rp2350-generic`，只补自己的设备表：
[`atk-dnr2350am/`](../atk-dnr2350am/README.md)、[`rpi-pico2/`](../rpi-pico2/README.md)。
RISC-V 一侧的同一层是 [`rp2350rv-generic/`](../rp2350rv-generic/README.md)。

## 硬件与启动链

| 项 | 值 |
| --- | --- |
| CPU | 双 Cortex-M33（ARMv8-M Secure），PLL_SYS 150 MHz；镜像默认单核，没有 `-smp` 目标 |
| SRAM | 520 KiB @ `0x20000000`：前 128 KiB 内核 `.data/.bss/栈`，其后 384 KiB 任务窗口 |
| Flash | **不声明**（`fs_backing = embedded`）：内核照样从 flash 里 XIP，但文件系统不落在上面 |
| 启动 | ROM 自己配 XIP，扫前 4 KiB 找 **PICOBIN IMAGE_DEF**（`image_def.S`），再从 flash 偏移 0 的向量表取 MSP/PC；没有第二级加载器 |
| UF2 | family `0xe48bff59`（ARM Secure EXE），产物 `build/tinyos-rp2350-generic.uf2`（约 912 KiB（934,400 B）） |
| 文件系统 | RAM disk，300 KiB（75 块 / 600 扇区），掉电即失 |
| 任务窗口 | 4 KiB 粒度，384 KiB 分出 96 个窗口；栈 4 KiB、栈下护栏 4 KiB |
| 内存保护 | **有**：PMSAv8 MPU（`TOS_ARM_MPU_V8`），代码 RO+X、数据/堆 RW、栈 RW、栈下护栏；第一次任务切换时打开 |
| 内核堆 | 60 KiB（`targets/rp2350-generic.conf`） |

启动顺序：

```
复位 → ROM 配 XIP、校验 PICOBIN → 向量表取 MSP/PC → _start
     → 拷 .data、清 .bss
     → plat_board_early：关 SAU（全 Secure）、开 clk_peri、XOSC、PLL_SYS 150 MHz
     → 控制台 / 看门狗 → [A]..[E] → banner → msh
```

两处顺序不是随便排的，都是真机上踩着 bug 定下来的：

- **SAU 必须在任何 SRAM 取指之前关掉**。ROM 交接时 SAU 是开的、划了八块；本镜像没有
  Non-secure 世界，任何一块被标成 Non-secure 都会让 Secure 取指落进
  invalid-transition SecureFault。Pico 2 上的症状是"恰好在切 `clk_sys` 时静默死亡"，
  因为切换代码在 `.data.ramfunc` 里（issue #7）。
- **切 `clk_sys` 的那几条指令必须在 SRAM 里**（`.data.ramfunc`）。在 flash 里切，
  切换瞬间的取指要重新对齐 flash 时序，W25Q32 有余量、W25Q64 没有。
- **内核完全不碰 QMI**。ROM 已经把这颗 flash 配成 EBh 四线，实测时序在 150 MHz 下
  可用；早期给 Pico 2 写的 `xip_setup = boot2-w25q080` 反而把 XIP 弄坏，已删除。

## 构建

```bash
go run ./cmd/tos build -target rp2350-generic
# -> build/kernel-rp2350-generic.elf
# -> build/tinyos-rp2350-generic.uf2

go run ./cmd/tos build all          # 连同其它目标一起构建，不在源码树外改配置
```

默认外设特性 `pio`；`board.conf` 的 `[user] exclude` 去掉了 WiFi/网络和面板程序
（`screen`/`lcdconsole`/`splash`/`fb`），因为基线没有这些硬件。

## 烧写与看日志

没有 `tos run`，只有 BOOTSEL 盘：

```bash
python scripts/rp2350/flash.py --list                       # 列串口 / CDC / 可移动盘
python scripts/rp2350/flash.py --uf2 build/tinyos-rp2350-generic.uf2 --smoke
python scripts/rp2350/flash.py --uf2 build/tinyos-rp2350-generic.uf2 --smoke -p COM19
python scripts/rp2350/flash.py -p COM19 --no-flash --smoke  # 只看串口，不烧写
```

`--port auto` 按 VID/PID 找 CDC（`2e8a:000a`），不写死 COM 号。`--smoke` 在看到
`tos> ` 后跑 `echo / ls / free / uptime / cat /proc/uptime / gpio / reboot`。

板子已经跑着 TinyOS、而且固件支持 ROM 复位时，可以不按键进 BOOTSEL：

```bash
python scripts/rp2350/bootsel_poke.py COM27 20 --dtr   # CDC 上发 Ctrl-D，走 ROM 的 BOOTSEL 入口
```

## 控制台

- **USB CDC**（`2e8a:000a`）和 **UART0**（GP0=TX / GP1=RX，115200 8N1）同时是控制台，
  两路都能输入。UART0 是芯片自己的脚，不经过任何板级外设，所以 bring-up 时最可信。
- 基线**没有 LED、没有按键**：`/dev/led0`、`/dev/key0` 都不存在（设备表是空的，
  `plat_board_devs()` 返回 0 项）。内核 1 Hz 心跳仍会翻转硬编码的 **GP3**（ATK 的
  LED 接法），板上没接就没有可见现象——判活只能看串口。
- `cat /proc/dev` 是这张（空的）设备表；`cat /proc/cpuinfo` 看核数与时钟。

## 文件系统：RAM disk

`fs_backing = embedded` 意味着 littlefs 镜像被链接进内核（`ramdisk_sectors = 600`，
300 KiB），`ramdisk_init()` 直接把它当块设备挂载。好处是**不假设板上有什么 flash**，
坏处是掉电即失、`df` 只有 300 KiB。

要持久化就在板级 `board.conf` 里声明 flash，比如
[`rpi-pico2/board.conf`](../rpi-pico2/board.conf)：

```ini
fs_backing = flash
flash_bytes = 8m
lfs_offset = 512k
```

`plat_lfs_blocks()`（`kernel/ramdisk.c`）会在挂载前用 JEDEC RDID 实测容量：
**探测值比 board.conf 小就用探测值**（防止超出实际器件），比它大则不采信、按配置走
并打印一次警告（ROM 的 `FLASH_DEVINFO` 默认 16 MiB 是上界不是测量值）。

## 这块板用来做什么

它是**新板 bring-up 的第一步**：把 `rp2350-generic` 刷上去，能到 shell 就说明硅片、
时钟、控制台、tick、异常处理都没问题，剩下的事情全在板级引脚上。之后建
`board/<你的板>/`，写两个文件即可：

```ini
# board/<你的板>/board.conf
[board]
schema = 1
chip = rp2350
variant = <你的板>
base = rp2350-generic
peripherals = gpio, uart, usb, spi, i2c, pwm, dma, pio
fs_backing = embedded
ramdisk_sectors = 600
drivers = drivers/gpio/rp2350, drivers/spi/rp2350, drivers/usb/rp2350, drivers/pio/rp2
```

加一份 `board_data.c`（设备表，见 `board/rpi-pico2/board_data.c`），在
`targets/` 里加一个只写 `board`/`arch`/`heap` 的 conf，就能构建了。

## 移植进度

保守口径：✅ = 真机确认过；◐ = 代码已实现、构建通过，但还没有独立验收；☐ = 未做。

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| 构建 + UF2（PICOBIN / IMAGE_DEF / family） | ✅ | `tos build all` 全目标通过（18/18）；产物约 912 KiB（934,400 B）；构建期校验 IMAGE_DEF 魔数、MSP、reset 的 Thumb 位 |
| 冷启动到 shell | ◐ | 本 target 没有独立验收卡；同一份 `platform.c` 在 ATK（W25Q32）与 Pico 2（W25Q64）上都到过 shell |
| USB CDC + UART0 双控制台 | ◐ | 同上，控制台代码是这块板定义的，两块具体板实测 |
| littlefs on RAM disk | ◐ | 同一 `ramdisk_init()` 路径在 QEMU 与嵌入式镜像上覆盖；本镜像未单独跑 `dd` |
| PMSAv8 MPU（4 KiB 窗口） | ◐ | `prot` 的四个用例在 Pico 2（同 4 KiB 粒度）上全过，本 target 未单独跑 |
| tick / 看门狗 / `reboot`（ROM 入口） | ◐ | 同上；`reboot` 走 ROM 的 BOOT_TYPE_NORMAL，和上电同一条路径 |
| PIO（3 块 / 12 个 SM） | ◐ | 驱动与用户态汇编器随镜像编译；实机判定在 ATK/Pico 2 上做 |
| 双核 | ☐ | 没有 `rp2350-generic-smp`；双核在具体板上验（`atk-dnr2350am-smp` 等） |

## 已知限制

- 没有 LED/按键：心跳、`/dev/led0`、`/dev/key0` 都不可用，串口是唯一判活手段。
- RAM disk 掉电即失；要持久化得在板级 conf 里声明 `flash_bytes` / `lfs_offset`。
- 单核镜像；`smpbench` 这类程序能跑但没有第二个核。
- 前 128 KiB SRAM 固定给内核，任务窗口只剩 384 KiB。
- MPU 是**真保护**：`prot` 在这里会真的拦下越界，别在产线上当玩具跑。

## 相关文件

- `board/rp2350-generic/{platform.c,layout.c,kernel.ld,arm_memmap.h,image_def.S,rp2350_xosc.h}`
- `board/rp2350-generic/board_devs.c`：设备表驱动的 LED/按键（本层没有表，故不生效）
- `arch/arm/`：异常入口、MPU（PMSAv8）、SMP 启动、时钟与串口公共代码
- `drivers/gpio/rp2350/`、`drivers/spi/rp2350/`、`drivers/usb/rp2350/`、`drivers/pio/rp2/`
- `internal/build/uf2.go`：`emitRP2350UF2()`、family / IMAGE_DEF / MSP 校验、E10 块
- `scripts/rp2350/flash.py`、`scripts/rp2350/accept33.py`、`scripts/rp2350/bootsel_poke.py`
- `docs/PICO2-VS-ATK-BOOT.md`：同一颗硅片上两块板的启动差异
