# Raspberry Pi Pico 2 / RP2350（`-target rpi-pico2` / `rpi-pico2-smp`）

同一颗 RP2350A，换到 Raspberry Pi 自己的板子上。本目录只替换**设备表**：`platform.c`、
`layout.c`、链接脚本、PICOBIN 头全部来自
[`rp2350-generic`](../rp2350-generic/README.md)（`base = rp2350-generic`）。
RISC-V 一侧是 [`rpi-pico2-rv32/`](../rpi-pico2-rv32/README.md)。

与 ATK 开发板的差别全是**板级事实**，不是代码分支：

- LED 在 **GP25，高电平点亮**（ATK 是 GP3、低电平）；
- **没有用户按键**：BOOTSEL 接的是 flash 片选，不是 GPIO，所以设备表里没有 `key0`；
- **没有 ST7789 屏**，没有 TF 卡座 → 镜像里排除 `screen`/`lcdconsole`/`splash`/`fb`；
- flash 是 **W25Q64，8 MiB**（本仓库实测的这块是 8 MiB 克隆板；官方 Pico 2 是 4 MiB
  W25Q32，见下面"4 MiB 与 8 MiB"）。

## 硬件与启动链

| 项 | 值 |
| --- | --- |
| CPU | 双 Cortex-M33（ARMv8-M Secure），PLL_SYS 150 MHz；`rpi-pico2-smp` 才拉 core 1 |
| SRAM | 520 KiB @ `0x20000000`：前 128 KiB 内核，其后 384 KiB = 96 个 4 KiB 窗口 |
| Flash | 8 MiB QSPI（W25Q64，实测），XIP @ `0x10000000`；官方板 4 MiB（W25Q32） |
| 启动 | 没有第二级加载器：ROM 配好 XIP 后扫前 4 KiB 找 **PICOBIN IMAGE_DEF**，从 flash 偏移 0 的向量表取 MSP/PC |
| UF2 | family `0xe48bff59`，产物 `build/tinyos-rpi-pico2.uf2`（约 2 MiB：内核 + 300 KiB littlefs 映像） |
| 文件系统 | littlefs @ flash `0x80000`，跑到 flash 末尾；8 MiB 板上 `df` = **7,864,320** |
| 任务窗口 | 4 KiB 粒度，96 个窗口；栈 4 KiB、栈下护栏 4 KiB |
| 内存保护 | **有**：PMSAv8 MPU（`TOS_ARM_MPU_V8`），代码 RO+X、数据/堆 RW、栈 RW、护栏 |
| 内核堆 | 60 KiB（`targets/rpi-pico2.conf`） |
| 板载设备 | `led0`（GP25，高电平亮）、`uart0`（GP0/GP1）、`usb0`、`adc0..3`（GP26..29）、`flash0` |

UF2 在 `0x10ffff00` 处带一块 256 B 的 **RP2350-E10 workaround**（`0xef` 填充 +
`RP2_IGNORE_BLOCK` 扩展标签 `0x9957e304`），让 A2 版 ROM 跳过它而不是当成有效数据。

启动顺序和芯片基线一致，Pico 2 的两次"静默死亡"都是在这条链上定位的：

```
复位 → ROM 配 XIP（EBh 四线，SFDP 选的时序）、校验 PICOBIN → 向量表 → _start
     → 拷 .data / 清 .bss
     → plat_board_early：关 SAU → 开 clk_peri → XOSC → PLL_SYS 150 MHz（切换在 SRAM 里）
     → 控制台 → [A]..[E] → banner → msh
```

根因记录在 `docs/OPEN-ITEMS.md` §1b 与 `docs/PICO2-VS-ATK-BOOT.md`：① 在 flash 里切
`clk_sys`，取指要重新对齐 flash 时序，W25Q64 没有余量；② 早期给这块板加的第二级加载器
（`boot2-w25q080`）把 ROM 已经配好的 QMI 又写了一遍，弄坏 XIP —— 现在内核完全不碰 QMI；
③ 首行串口用固定 12 MHz 猜分频，于是前两个故障都表现为"零字节"。

## 构建

```bash
go run ./cmd/tos build -target rpi-pico2
# -> build/kernel-rpi-pico2.elf
# -> build/tinyos-rpi-pico2.uf2

go run ./cmd/tos build -target rpi-pico2-smp        # 双核
go run ./cmd/tos build -target rpi-pico2 -config configs/rpi-pico2-quiet.conf   # 去掉 [A]..[E]
```

每个 target 的优化级别是分开的：`opt = -Os` 给内核（比 `-O2` 小 30%，CoreMark
487 vs 478、`smpbench` 2.00–2.05x），`user_opt = -O2` 留给用户程序——`TOS_OPT`
同时压用户程序时 CoreMark 会从 478 掉到 289。`board.conf` 默认
`features = pio, rir, kthreads, usb_pump`。

`configs/rpi-pico2-rir.conf` 是板上自带工具链（`csc` + `rvm` + `rcc`）的镜像，
`configs/rpi-pico2-quiet.conf` 关掉启动探针（**只在板上已经通过带 trace 的镜像之后用**，
`accept33.py` 要读 `[A]..[E]`）。

## 烧写与看日志

把 UF2 拖进 BOOTSEL 盘，或者用脚本（会先开串口再拷包，不丢启动字符）：

```bash
python scripts/rp2350/flash.py --list
python scripts/rp2350/flash.py --uf2 build/tinyos-rpi-pico2.uf2 --smoke
python scripts/rp2350/flash.py -p COM19 --no-flash --smoke        # 只看串口
python scripts/rp2350/bootsel_poke.py COM27 20 --dtr              # Ctrl-D：不按键进 BOOTSEL
```

- `--port auto` 按 VID/PID 找 CDC（`2e8a:000a`），不写死 COM 号。
- **刷完别立刻 close 再重开串口**：板子很快就到 shell，重开的那一下会把 banner 丢掉。
- 板上没有按键，BOOTSEL 只能按 BOOTSEL 键进；TinyOS 起来之后 `Ctrl-D` 走 ROM 的
  BOOTSEL 入口（`bootsel_poke.py` 就是发这个），比按按钮方便。

## 控制台与板载设备

- **USB CDC**（`2e8a:000a`）是主控制台；**UART0**（GP0/GP1，115200 8N1）同时输出，
  两路都能输入。板子上的 CH343 把 UART0 接到第二个串口口（本机是 COM19）。
- LED：`echo 1 > /dev/led0` 点亮 GP25（高电平亮，走设备表）。用户态一碰 LED，内核心跳
  就停手（`led_owned`），不会和你的命令抢。
- 但**内核自己的 1 Hz 心跳仍写死 GP3、低电平有效**（ATK 的接法，`platform.c` 的
  `RP_LED_GPIO`），所以在这块板上看不到心跳灯——判活着串口或 `/dev/led0`。
  这是 issue #27 的遗留项，不是板子坏了。
- `cat /proc/dev` 看整张设备表；`screen`/`fb`/`lcdconsole` 不在镜像里（没有屏）。

## 4 MiB 与 8 MiB

官方 Pico 2 焊的是 **W25Q32（4 MiB）**，本仓库开发用的这块是 8 MiB 的克隆板，
`board.conf` 因此写 `flash_bytes = 8m`。这不影响把镜像刷到 4 MiB 的板子上：

- `plat_lfs_blocks()` 挂载前用 JEDEC RDID 实测容量，**探测值比配置小就用探测值**，
  所以 4 MiB 板上文件系统会是 3.5 MiB（`df` = 3,670,016），不会越过器件末尾；
- 探测值比配置大时不采信（ROM 的 `FLASH_DEVINFO` 默认 16 MiB 是上界），按配置走并
  打印一次警告；
- 想按官方 4 MiB 板出镜像，把 `board.conf` 的 `flash_bytes` 改成 `4m` 重新构建即可；
  `flash_bytes` 只决定 littlefs 的运行时 geometry，内核代码本身不依赖它。

`scripts/rp2350/accept33.py` 的 `df` 期望值按 8 MiB 板写成 `7864320`；在 4 MiB 官方板上
跑这一项会判不符（其余项不受影响），把 `IMAGES` 里该镜像的 `df` 改成 `3670016` 即可。

## 验收怎么跑

```bash
python scripts/rp2350/accept33.py --image rpi-pico2 -p COM19
python scripts/rp2350/accept33.py --image rpi-pico2-smp --uf2 build/tinyos-rpi-pico2.uf2 -p COM19
python scripts/rp2350/heartbeat.py -p COM27 --target rpi-pico2
```

`accept33.py` 用 `--image` 选每张镜像的期望（核数、RV 标记、`df`），镜像里没有的程序
记 skipped 而不是 failed；默认端口是 UART0，因为它是一颗独立的 USB 串口芯片，
板子进 BOOTSEL 时仍然在。

## 移植进度

保守口径：✅ = 真机确认过；◐ = 代码已实现、构建通过，但还没有完整验收；☐ = 未做。
最近一次四镜像全量回归见 `docs/acceptance/2026-11-24-pico2-matrix-05a0e18.md`。

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| 冷启动到 shell（无第二级加载器） | ✅ 实机 | 冷启动直接到 `tos> `；SAU/PLL/串口分频三处修复后不再 HardFault |
| USB CDC + UART0 双控制台 | ✅ 实机 | `heartbeat` 6/6（两个通道都算）；长输出已按 CDC 读侧流控修过（#46） |
| littlefs on flash（ROM flash API） | ✅ 实机 | `dd_stress` **200/200**（8 MiB 板）；`reboot` 后文件仍在 |
| PMSAv8 MPU（4 KiB 窗口） | ✅ 实机 | `prot` 五例（rodata/heap/stack/null/dev）全部 MemManage 后回到提示符 |
| 双核 SMP（`rpi-pico2-smp`） | ✅ 实机 | `smptest` 11/11；`smpbench` **2.05x**；`reboot_soak` 20/20（0.40–0.41 s） |
| `reboot`（ROM 入口，先停另一核） | ✅ 实机 | 不停从核会留下"CDC 能开、永远不答话"的板子；`plat_reboot()` 先 park 再走 ROM |
| 板上工具链（csc → rvm → rcc） | ✅ 实机 | `cover`/`state`/`ready` 三组全链一致（`configs/rpi-pico2-rir.conf`） |
| PIO（3 块 / 12 个 SM） | ◐ | 驱动与用户态汇编器随镜像编译；17/17 的实机判定记录在 ATK / RP2040 上（`pio_accept.py`），本板未单独跑 |
| LED / ADC / `/proc/dev` | ◐ | `/dev/led0` 走设备表（GP25 高电平）；内核心跳仍打 GP3，本板不可见（#27） |
| RV32 镜像在这块板上 | — | 见 [`rpi-pico2-rv32/`](../rpi-pico2-rv32/README.md) |

## 已知限制

- 没有按键、没有屏：`key0`/`lcd0`/`fb0` 都不存在；`screen`/`fb` 已从镜像排除。
- 内核 1 Hz 心跳固定在 GP3（ATK 的 LED），Pico 2 上看不到；`/dev/led0` 才是这块板的灯。
- 512 KiB 里 128 KiB 固定给内核；littlefs 从 `0x80000` 起，内核必须留在前 512 KiB。
- 内核堆 60 KiB，与 ATK 相同（历史上按"exec 同时持有压缩+解压缓冲"定的）。
- `prot` 是真保护，别在产线上乱跑。
- 4 MiB 板上跑 `accept33.py` 的 `df` 一项要改期望值（见上）。

## 相关文件

- `board/rpi-pico2/board.conf`（板级事实）、`board_data.c`（设备表）
- `board/rp2350-generic/`：`platform.c`、`layout.c`、`kernel.ld`、`image_def.S`、`board_devs.c`
- `board/rpi-pico2-rv32/`：同一块板的 Hazard3 镜像
- `internal/build/uf2.go`：`emitRP2350UF2()`、family / IMAGE_DEF / E10 块
- `scripts/rp2350/{flash.py,accept33.py,heartbeat.py,bootsel_poke.py}`
- `docs/PICO2-VS-ATK-BOOT.md`、`docs/OPEN-ITEMS.md` §1b
