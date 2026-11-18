# 为什么 ATK 能跑、Pico 2 不能：RP2350 启动差异分析（2026-11-18）

结论先行：**两块板的硅片、ROM、内核代码完全一样，差别在三处"边界条件"** ——
flash 部件（W25Q32 4 MiB vs 克隆板 W25Q64 8 MiB）、Pico 2 额外启用的第二级
加载器（`xip_setup = boot2-w25q080`）、以及早期串口把"看不见"当成了"跑不到"。
真正让 Pico 2 停在半路的，是**时钟切换瞬间的那一次 flash 取指**和**第二级加载器
对 flash/QMI 的改写**；ATK 对前者有余量、根本没有后者。

修复提交 `cff442f`，验收 `docs/acceptance/2026-11-18-pico2-boot-rp2350-cff442f.md`。

---

## 1. 两块板到底差什么

| | ATK-DNRP2350AM | Pico 2（克隆板） |
|---|---|---|
| SoC | RP2350A（Cortex-M33，Secure） | **同一颗**，同一 ROM |
| Flash | W25Q32 类，4 MiB | **W25Q64FV，8 MiB**（ROM 通过 SFDP 选模式/时序） |
| 内核镜像 | 同一份 `board/rp2350-generic` 代码 | 同一份代码 + `base = rp2350-generic` |
| `board.conf` 差异 | `flash_bytes = 4m`，**无 `xip_setup`** | `flash_bytes = 4m`，**`xip_setup = boot2-w25q080`** |
| 板级外设 | ST7789、TF 卡、用户键，LED 在 **GP3**（低电平点亮） | 仅 RGB LED 在 **GP25**（高电平点亮），无屏无卡 |
| 早期 LED | `led_set()` 硬编码 GP3 → 可见 | `led_set()` 仍打 GP3 → **Pico 2 上不可见**（遗留问题，另立 issue #27） |

`xip_setup` 是关键：它让 `arch/arm/start.S` 在 `.data` 拷贝**之前**，把 256 字节的
`boot2_w25q080.S` 拷到 SRAM 并 `blx` 过去执行。ATK 的镜像里没有这段。

## 2. 启动链（基线与修复后）

```
复位 → ROM：配 XIP（用 SFDP 找到的模式/时序）、扫前 4 KiB 找 PICOBIN、
        从 0x10000000 取 MSP/PC → _start
_start → [xip_setup 时] 拷 boot2 到 SRAM 并执行     ← Pico 2 多出来的一步
        → 拷 .data、清 .bss → kmain → plat_board_early
plat_board_early → clocks_init（XOSC / clk_ref / clk_peri）
                 → pll_sys_init（PLL 150 MHz + 切 clk_sys）
                 → 串口/灯/看门狗 → [A][B][C][D] → banner → msh
```

## 3. 故障链：同一条链，两块板走到不同的地方

### 3.1 第一处：切 clk_sys 的那一次取指（板级余量问题）

`pll_sys_init()` 基线版是这样写的（在 **flash** 里执行）：

```c
mmio_write32(CLOCKS_BASE + CLK_SYS_DIV_OFFSET,  CLK_DIV_1);
mmio_write32(CLOCKS_BASE + CLK_SYS_CTRL_OFFSET, CLK_SYS_CTRL_SRC_AUX | ...);
// 下一条指令的取指已经在 150 MHz 下、走 XIP
```

QMI 的采样延迟以 **clk_sys 周期**计，而 ROM 是按**启动时钟**（12 MHz）调的。
真机实测 ROM 交接时的 QMI 状态（独立测试镜像读出）：

```text
M0_TIMING = 0x60007203   → cooldown 1, RXDELAY 50, CLKDIV 3
M0_RCMD   = 0x000000eb   → 前缀 0xEB
M0_RFMT   = 0x000492a8   → 四线地址/四线数据, 4 个 dummy cycle, 前缀 8 bit
⇒ flash 已经在 EBh 四线读模式，SCK = clk_sys/3
```

**这块 flash 的时序在 150 MHz 下其实是够用的**——切换后实测：

```text
M000249f1                        ← FC0 实测 clk_sys = 150 001 kHz
stage4: clk_sys=0x08f0d180  ctrl=0x1 sel=0x4
stage5: flash again, sum=0xb5ad668c  SAME -> XIP OK
```

切换前后对同一段 4 KiB flash 求校验和完全相同。所以**"150 MHz 下 XIP 一定会坏"
这个前提是错的**，项目里此前基于它的判断（包括那套 boot2）也跟着错了。

那为什么 Pico 2 会死？差别在**切换的一瞬间**：写 `CLK_SYS_CTRL` 之后，紧接着的
取指要在新时钟下重新同步采样，而这次取指发生在 QMI/时钟都刚变化的窗口里。
这个窗口的余量由 flash 部件的实际输出延迟决定：

- **ATK（W25Q32）**：余量足够，切换瞬间那几次取指仍然读对 → 继续启动；
- **Pico 2（W25Q64 克隆）**：余量不足，瞬时读到垃圾 → 取指错 → 双 fault
  → `PC=0xeffffffe`（issue #7 A 轨 SWD 现场就是这一条）。

**修复**：把这次切换挪进 `.data.ramfunc`（`clk_sys_to_pll_sys()`），切换前后都在
SRAM 取指；等 `CLK_SYS_SELECTED` 落定后，第一次 flash 取指已经在稳定状态下发生。
两块板都因此不再依赖这个余量。

> 证据等级：**机制解释 + 强间接证据**。直接隔离"flash 里切换 vs SRAM 里切换"单变量
> 的实验没做（ATK 上做才有意义，因为它基线能跑）；下面 §6 给出补测方法。

### 3.2 第二处：`boot2` 第二级加载器（只有 Pico 2 有）

A 轨最初失败后，为了"把 flash 切到四线连续读"给 rpi-pico2 加了
`xip_setup = boot2-w25q080`。这个 256 字节 blob：

1. 读 SR2（0x35），不匹配就写 `0x01` 打开 QE 位；
2. 把 `M0_TIMING` 改成 `CLKDIV 2 / RXDELAY 2`；
3. 用一条 0xEB + 模式位的读进入连续读模式，再把前缀长度清零。

问题在于 **ROM 已经把这颗 flash 配好了**（上面那份 EBh 四线配置），而这段 blob 是在
ROM 的配置之上**再做一次**，其中还包含对 flash 状态寄存器的写。真机 A/B（`_start` 里
插 GP25 探针）结果是决定性的：

| 构建 | 现象 | 结论 |
|---|---|---|
| 带 boot2 | 只闪 2 下（boot2 之前那组），之后全灭 | 死在这段 SRAM 代码/它之后的第一次取指 |
| 不带 boot2 | 2 + 4 + 8 三组全出现 | `_start`、`.data` 拷贝、进 kmain 都正常 |

而对 **ATK 的 W25Q32**，这段 blob 根本没被编进镜像（没有 `xip_setup`），所以从来
没有机会破坏 XIP。

**修复**：`board/rpi-pico2/board.conf` 不再启用它；并且内核**任何地方都不再写 QMI**
（见 3.1 的实测：ROM 的时序够用，不需要重调）。`arch/arm/start.S` 里保留这条路径与
注释，留给确实需要四线的板子，并写明"启用前必须真机验证"。

> 证据等级：**已测量**。带/不带 boot2 的 A/B、boot2 字节与 pico-sdk 源码逐字节一致、
> ROM 时序在 150 MHz 下的校验和一致，都有记录。

### 3.3 第三处：早期串口波特率是猜的（把前两处伪装成"死在极早期"）

基线 `plat_board_early()` 的顺序是：

```c
clocks_init();
pll_sys_init();          // 切到 150 MHz
wdt_enable();
unreset(IO/PADS/UART0);
led_init(); led_set(1);  // 打 GP3 → Pico 2 上看不见
uart_init();             // ← 用 g_clk_hz = 12 MHz 的**猜测**算分频
early_puts("\r\n[A] clocks+reset ok\r\n");
khz = measure_clk_khz(); // 这里才第一次测量
uart_init();             // 重新分频
early_puts("[B] clk=0x...");
```

`clk_peri` 跟着 `clk_sys` 走，PLL 成功时这里已经是 **150 MHz**，而分频按 12 MHz 算
→ 实际波特率是 115200 的 12.5 倍。于是：

- **ATK**：`[A]` 这一行是乱码，但没人依赖它（大家看 `[B]` 起）；`[B]` 前已经重新
  分频，所以后续日志、shell 全部正常。**这个缺陷在 ATK 上一直存在，只是没有后果。**
- **Pico 2**：因为它死得更早（3.1/3.2），`[B]` 根本没机会打印，于是整块板
  **一个可读字节都没有**。诊断时"没有输出"被当成"连 `_start` 都没进"，
  整个排障方向因此跑偏了好几轮（包括我这次一度以为是 PLL 坏了）。

独立测试镜像把这个说清楚了：同样的切换、同样的 flash，先用 FC0 测频率再算分频，
输出立刻变成完全可读的 `M000249f1` / `stage4 ... clk_sys=0x08f0d180`。

**修复**：`clk_sys` **先测后打**（ARM 端口），FC0 的参考频率按 `clk_ref` mux 取
（XOSC 12 MHz / ROSC 6.5 MHz，而不是写死 12 MHz）；调 `clk_peri` 之前先
`plat_console_flush()`，避免尾巴用新波特率发出。RV32 端口早就有等价做法
（`[A] pre-pll ok` + flush），这次把 ARM 端口补齐并对齐。

> 证据等级：**已测量**。修复前"乱码头 + 可读尾 + 中途截断"，修复后
> `[A] clocks+reset ok  clk_sys=00b71ee8` 完整可读。

### 3.4 第四处：SAU / VTOR 接管得太晚（防御性加固）

ROM 交接时 `SAU_CTRL = 1`、`SAU_TYPE = 8`（8 个 region），MPU 是关的
（`MPU_CTRL = 0`）。这块板是纯 Secure 镜像，任何被 SAU 标成 Non-secure 的
SRAM/flash 区域，都会让 **Secure 取指**落进 invalid-transition SecureFault。
基线把 `ARM_SAU_CTRL = 0` 放在 `[C]`，也就是 PLL 之后。

在 ATK 上，`[C]` 之前唯一的 SRAM 执行是 `wdt_enable()`（RAMFUNC），它能跑，
说明 ROM 留下的 SAU 配置在这颗板上并没有把 SRAM 标成不可执行——**所以不能简单说
"SAU 让 SRAM 不能执行"**。但这项接管本来就该发生在"第一次从 SRAM 取指"之前：
修复后 `clk_sys_to_pll_sys()` 是启动路径上最早的 SRAM 执行点，把它放在 SAU 接管之后，
这一类失败就与"谁先跑 SRAM"无关了。

**修复**：`plat_board_early()` 最开头写 `VTOR` 和 `SAU_CTRL = 0` + `dsb/isb`，
把后面 `[C]` 处的重复代码删掉（保留注释指向新位置）。

> 证据等级：**防御性，未单独隔离**。它没有坏处，也不能证明是 Pico 2 的直接死因
> （§6 给出隔离方法）。

## 4. 为什么这些失败看起来都像同一件事

四个假象叠在一起，任何一个都足以把方向带偏：

1. **所有异常处理都在 flash 里**。XIP 一坏，fault handler 自己的取指也坏 →
   双 fault → ROM 的 lockup 向量 `0xeffffffe`，现场只剩一个 PC。
2. **早期串口是猜的分频**。死在 `[B]` 之前的故障，输出是乱码或零字节，
   和"根本没启动"无法区分。
3. **早期 LED 硬编码 GP3**（ATK 的脚）。Pico 2 上 `led_set(1)` 什么也看不见，
   所以"灯不亮"被当成"没跑到"，实际是"点错脚"（issue #27）。
4. **`_start` 里的 boot2 探针本身会破坏 XIP**，让后续探针也失效，
   于是"第 2 组探针没出现"既可能是"死在 boot2"，也可能是"死在 boot2 之后的第一步"。

## 5. 修复后两块板的行为

同一份 `board/rp2350-generic` 代码，两边都只切 mux、不碰 QMI、先测频率：

| | ATK（基线即可跑） | Pico 2（修复后） |
|---|---|---|
| 启动时钟 | 12 MHz → 测 FC0 | `0x00b71ee8` = 11 997 928 Hz |
| 切换后 | 150 MHz | `0x08f0d180` = 150 000 000 Hz |
| clk_usb | 48 MHz | `0x0000bb80` = 48 000 kHz |
| 早期日志 | `[A]` 乱码（历史遗留）→ 修复后同样可读 | 完整可读 |
| shell | 一直可用 | `tos>` + `ps`/`ls`/`uptime`/`wfile`/`crash` 全通过 |
| UF2 刷写 | 拖入即启动 | 拖入即启动（实测 `uptime` 已 49.9 s，未按 RUN） |

对照实验：用云端基线 `ee3c6d8` 构出的同一目标镜像，即使按 RUN 也**零输出**。

## 6. 还没隔离的部分（给出补测方法）

| 待证 | 实验 | 预期 |
|---|---|---|
| "flash 里切 mux" 是否单独足以让 Pico 2 死 | 在修复版上把 `clk_sys_to_pll_sys()` 临时改成普通函数（链接在 flash），其余不动 | 若仍能启动，说明 3.1 只是余量问题而非必然；若死，3.1 得到直接证明 |
| SAU 是否单独致命 | 修复版里把 VTOR/SAU 接管移回 `[C]`，其余不动 | 若仍能启动，说明 3.4 纯属加固 |
| boot2 的哪一步破坏 XIP | 用只有 `M0_TIMING` 改写的精简 blob（不写 SR2） | 预期：仍坏 → 破坏来自 QMI 改写；若好了 → 来自 SR2 写 |
| ATK 是否也踩 3.1 | ATK 上把切换放回 flash 并反复冷启动统计 | 预期：偶发失败（余量问题在两块板上都存在） |

## 7. 复现

```sh
# 构建
go run ./cmd/tos build -target rpi-pico2     # 修复版
git worktree add /tmp/base ee3c6d8
(cd /tmp/base && go run ./cmd/tos build -target rpi-pico2)   # 基线（对照）

# 刷入（BOOTSEL 拖 UF2，ROM 自动重启）
# 串口：UART0 GP0/GP1，115200 8N1
#   修复版：[A] clocks+reset ok  clk_sys=00b71ee8 → [B] clk_sys=08f0d180 → ... → tos>
#   基线版：无任何输出（按 RUN 也一样）
```
