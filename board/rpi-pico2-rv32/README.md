# Raspberry Pi Pico 2 / Hazard3 RISC-V（`-target rpi-pico2-rv32` / `rpi-pico2-rv32-smp`）

同一块 Pico 2，换到 **Hazard3（RV32IMAC）** 那两条 hart 上。本目录**只有一个
`board.conf`**：ISA 相关的 `platform.c`、链接脚本、PICOBIN 头来自
[`rp2350rv-generic`](../rp2350rv-generic/README.md)，板级数据（LED GP25、无按键/屏/卡、
8 MiB flash）来自 [`rpi-pico2`](../rpi-pico2/README.md)，两个 `base` 都用：

```ini
base = rp2350rv-generic, rpi-pico2
```

所以 ARM 侧和 RISC-V 侧**没有一份 C 文件重复**。ARM 镜像见
[`rpi-pico2/`](../rpi-pico2/README.md)。

## 硬件与启动链

| 项 | 值 |
| --- | --- |
| CPU | 双 Hazard3 hart（RV32IMAC），PLL_SYS 150 MHz；`rpi-pico2-rv32-smp` 才拉第二个 hart |
| SRAM | 520 KiB @ `0x20000000`：前 128 KiB 内核，其后 384 KiB = 24 个 **16 KiB** 窗口 |
| Flash | 8 MiB QSPI（W25Q64，本仓库这块；官方 Pico 2 是 4 MiB W25Q32），XIP @ `0x10000000` |
| 启动 | 与 ARM 侧同一套 PICOBIN；ROM 配 XIP、校验 IMAGE_DEF 后从向量表取栈/入口；无第二级加载器 |
| UF2 | family `0xe48bff59`，产物 `build/tinyos-rpi-pico2-rv32.uf2`（约 2 MiB） |
| 文件系统 | littlefs @ flash `0x80000` 到末尾；8 MiB 板上 `df` = **7,864,320** |
| 任务窗口 | 16 KiB 粒度，24 个；栈 4 KiB、护栏 4 KiB、`stack_align = 1`（PMP 的 NAPOT 对齐要求） |
| 内存保护 | **有**：PMP（NAPOT）；代码 RO+X、数据/堆 RW、栈 RW，堆与栈之间留洞 |
| 内核堆 | 32 KiB（`targets/rpi-pico2-rv32.conf`） |
| 板载设备 | `led0`（GP25，高电平亮）、`uart0`（GP0/GP1）、`usb0`、`adc0..3`（GP26..29）、`flash0` |

为什么 RV32 侧是 16 KiB 而 ARM 侧是 4 KiB、为什么带 `rvc = auto`（取指不能跨两个
PMP 条目）、为什么 `mret` 回 M 态的内核线程不受 PMP 约束 —— 都在
[`rp2350rv-generic` 的 README](../rp2350rv-generic/README.md) 里，这里不重复。

## 构建

```bash
go run ./cmd/tos build -target rpi-pico2-rv32
# -> build/kernel-rpi-pico2-rv32.elf
# -> build/tinyos-rpi-pico2-rv32.uf2

go run ./cmd/tos build -target rpi-pico2-rv32-smp            # 双 hart
go run ./cmd/tos build -config configs/rpi-pico2-rv32-quiet.conf        # 双 hart + 关 trace
go run ./cmd/tos build -config configs/rpi-pico2-rv32-rir.conf          # 板上工具链
```

优化级别与 ARM 侧同源：`opt = -Os`（内核）、`user_opt = -O2`（用户程序），
`features = pio, rir, kthreads, usb_pump`。

构建日志里出现下面几行是**正常的**（`rvc = auto` 在为跨页的程序换编码）：

```text
RVC bin/edit / pio / sh / csc / rcc / rvm: rebuilt without compressed instructions
```

注意 `rpi-pico2-rv32-rir.conf` 里 `csc`/`rvm`/`rcc` 只要 4 KiB 栈：16 KiB 窗口里放不下
16 KiB 栈再加别的东西，`stk`/`csa`/`py` 这类硬要 16 KiB 栈的程序在这个镜像里构建期
就会被 `mklfs --grain` 拒掉，而不是等 `exec` 时由内核拒绝。

## 烧写与看日志

```bash
python scripts/rp2350/flash.py --list
python scripts/rp2350/flash.py --uf2 build/tinyos-rpi-pico2-rv32.uf2 --smoke
python scripts/rp2350/accept33.py --image rpi-pico2-rv32 -p COM19
python scripts/rp2350/bootsel_poke.py COM27 20 --dtr   # 不按键进 BOOTSEL
```

- 默认端口用 **UART0**（本机 CH343 = COM19）：它是独立的 USB 串口芯片，板子进 BOOTSEL
  时仍然在。CDC 是 `2e8a:000a`，`--dtr` 可以用。
- 刷机后 **UART0 不一定有字节**，这是这块芯片上两个 ISA 都见过的现象；抓启动日志以
  CDC 为准，`flash.py` 默认先试 CDC。
- RV32 镜像的启动标记是 `[A]` 的**时钟快照**（FC0 实测 clk_sys），不是 ARM 侧的
  SAU/MPU 摘要；`accept33.py` 按 `rv=True` 选这套用词。
- 刷完别立刻 close 再重开串口，会把 banner 丢掉。

## 控制台与板载设备

- USB CDC（`2e8a:000a`）+ UART0（GP0/GP1，115200 8N1），两路都是控制台。
- `echo 1 > /dev/led0` 点 GP25（高电平亮，走设备表）；没有按键（`key0` 不存在）、没有屏。
  内核心跳仍写死 GP3（ATK 的接法），本板看不到——判活看串口。
- `cat /proc/dev` 看设备表，`cat /proc/cpuinfo` 看 hart 数与时钟。

## 验收与实测记录

四镜像同一天全量回归：`docs/acceptance/2026-11-24-pico2-matrix-05a0e18.md`。

```text
heartbeat            6/6
smptest              11/11（--func 6/7，缺 py）
dd_stress 200 轮     200/200
smpbench 1250000     2.00x
reboot_soak 20 次    20/20（0.41–0.56 s）
prot                 heap/stack/dev 三例 fault（rodata/null 按设计存活）
csc → rvm → rcc      cover/state/ready 三组全链一致
```

RV32 的故障用词与 ARM 不同：`prot` 打印的是 `load/store access fault`，ARM 是
`MemManage`。判据写在 `accept33.py` 的 `rv=True` 分支里，两张镜像共用一个脚本。

## 移植进度

保守口径：✅ = 真机确认过；◐ = 代码已实现、构建通过，但还没有完整验收；☐ = 未做。

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| 冷启动到 shell（PICOBIN，无第二级加载器） | ✅ 实机 | 与 ARM 侧同一条 ROM 路径；`[A]` 时钟快照在 150 MHz |
| USB CDC + UART0 双控制台 | ✅ 实机 | `heartbeat` 6/6；长输出按 CDC 读侧流控处理 |
| littlefs on flash（ROM flash API） | ✅ 实机 | `dd_stress` **200/200**；`reboot` 后文件仍在 |
| PMP 每任务窗口 + 堆/栈之间的洞 | ✅ 实机 | `prot heap/stack/dev` 三例 fault；`rodata`/`null` 按 PMP 语义存活 |
| 双 hart（`rpi-pico2-rv32-smp`） | ✅ 实机 | `smptest` 11/11；`smpbench` **2.00x**；`reboot_soak` 20/20 |
| `reboot`（先 park 另一个 hart） | ✅ 实机 | 不停从核会留下"CDC 能开、不答话"的板子；修复后 20/20（45 s 窗口 10/10） |
| 板上工具链（csc → rvm → rcc） | ✅ 实机 | `cover` 4284 text / 199 labels、`state` 5244 / 278、`ready` 2896 / 114 |
| RVC 逐程序回退 | ✅ 实机 | 6 个程序重建为无 RVC，`msh` 保留压缩；`rcc` 跨页取指问题（issue #44）由此修 |
| PIO | ◐ | 驱动随镜像编译；17/17 的实机判定记录在 ATK / RP2040 上 |
| LED / ADC | ◐ | 设备表与驱动已接，未单独做目视/读数记录 |

## 已知限制

- **PMP 只约束 U 态**：内核线程在 M 态跑，`prot` 对它们不生效（设计如此，见
  `arch/rv32/start.S` 的 `mret` 注释）。
- 7 块可用 PMP 条目（8 个里 5 号是 trap vector，8..10 被 bootrom 硬连），复杂窗口
  要靠梯形策略降级，启动日志会说明用了哪一级。
- 16 KiB 窗口装不下 16 KiB 栈 + 其它，`stk`/`csa`/`py` 这类程序在 `-rir` 镜像里被
  构建期拒绝。
- 没有按键/屏；UART0 刷机后可能没有输出，CDC 是主通道。
- 4 MiB 官方板上 `accept33.py` 的 `df` 期望要改成 `3670016`（同 ARM 侧）。

## 相关文件

- `board/rpi-pico2-rv32/board.conf`（**本目录唯一的文件**）
- `board/rp2350rv-generic/`：`platform.c`、`layout.c`、`kernel.ld`、`image_def.S`、`pmp_map.h`
- `board/rpi-pico2/board_data.c`：LED/ADC/flash 设备表（两个 ISA 共用）
- `arch/rv32/`：trap 入口、PMP CSR、`mret`、SMP 启动与 flash park
- `internal/build/codepage.go`、`internal/build/testdata/pmp_map_harness.c`
- `scripts/rp2350/{flash.py,accept33.py,bootsel_poke.py}`
- `docs/acceptance/2026-11-20-pico2-smp-boot-gate.md`：`reboot` 不停从核的现场
