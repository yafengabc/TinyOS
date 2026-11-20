# RV32 SMP：`reboot` 之后约一半的启动要 34 秒才出声（2026-11-20）

```text
commit:            待填（本轮是测量，不是修复）
change:            UART0 修好后用双通道测重启延迟，发现 RV32 SMP 独有的停顿
baseline:          none
target/config:     rpi-pico2-rv32-smp / defaults（对照：rpi-pico2-smp）
board/port/image:  Raspberry Pi Pico 2，CDC=COM27，UART0=COM19（CH343）
toolchain:         go 1.x, clang 23.1.4, python 3.13
host:              Windows 11 / EKSOFT
T0:                UART0 双向可用（命令走 UART0、回复也从 UART0 回来）
T1:                ARM 镜像 10 次 `reboot`：10/10 在 0.38 s 回来
T2:                RV32 SMP 镜像 10 次 `reboot`：6 次 0.50–0.57 s，4 次 **34.01 s**
T3:                34.01 s ≈ 2 × 16.78 s（`WATCHDOG_RELOAD = 0x00ffffff` µs）
T4:                暗窗口内 UART0 一个字节都没有，CDC 设备也不在总线上
T5:                阶段标记：停顿前那次启动到了 `BOOT_PHASE(14)`（USB init 之后）
T6:                那一刻 `clk_peri=0x10000800`、`UART_CR=0x00000301`，UART 编程是对的
logs:              build/gp0_watch.log（另一件事）；本节数据为串口摘录，未入库
notes:             未修复；仪表本身会改变命中率（TOS_DIAG 内），根因未定
```

## 现象

`scripts/rp2350/reboot_latency.py` 每秒读两个通道，重启后等 `tos> `：

```text
[ 1/8] uart=  34.01 s  ...   691 uart bytes
[ 2/8] uart=  34.00 s  ...   691 uart bytes
[ 3/8] uart=  34.00 s  ...   691 uart bytes
[ 4/8] uart=   0.50 s  ...   710 uart bytes
[ 5/8] uart=   0.51 s  ...   710 uart bytes
[ 6/8] uart=  34.03 s  ...   691 uart bytes
[ 7/8] uart=   0.51 s  ...   710 uart bytes
[ 8/8] uart=   0.51 s  ...   710 uart bytes
```

慢的那几次不是“晚一点出声”，是**中间整整 33.6 秒线上什么都没有**：
第一个字节就是完整的 `[A] pre-pll ok ...`，前面没有半句日志。CDC 侧同样在这段时间
不可见（闸门里的 `cdc=-`）。慢轮次总字节数比快轮次少 19 字节（691 vs 710），对应的
正是少打印的那几行之一，待确认。

## 34 秒是什么

`board/rp2350rv-generic/platform.c` 的 `WATCHDOG_RELOAD = 0x00ffffff`
= 16 777 215 µs = **16.78 s**；33.56 s 正好是**两个看门狗周期**。所以形状是：

1. 第一次 reset 之后那次启动在某个点卡住（没有任何输出）；
2. 16.78 s 时看门狗复位 → 第二次启动同样卡住；
3. 33.56 s 时再复位 → 第三次启动正常，于是 33.6 s 处突然出现完整启动日志。

## 卡在哪一步

`WATCHDOG_SCRATCH(3)` 写阶段号（`BOOT_PHASE(n)`），在 `plat_board_early` 入口先把它
读进 RAM（每个阶段会覆盖同一个 scratch，不先读走就只剩最后一次的信息）：

| 标记 | 位置 | 观察到 |
| --- | --- | --- |
| 1 | 进入 `plat_board_early` | |
| 2 | `clocks_init()` 之后 | |
| 3 | `wdt_enable()` 之后 | |
| 4 | `unreset(IO_BANK0\|PADS\|UART0\|TIMER0\|TIMER1)` 之后 | |
| 5 | `ticks_init()` 之后 | |
| 6 | `led_init()` 之后 | |
| 7 | `uart_init()` 之后 | **第一版仪表：慢轮次全是 7** |
| 8/9 | `[A]` 打印 / `plat_console_flush()` 之后 | |
| 10/11 | `pll_sys_init()` / 重新测频之后 | |
| 12 | `[B]` 打印之后（同时存 `clk_peri`、`UART_CR`） | |
| 14 | `usb_cdc_console_init()` 之后 | **细分后：慢轮次全是 14** |

`prev_phase=14` 与 `clk_peri=0x10000800`、`UART_CR=0x00000301` 说明：那次启动
**已经把 UART 配好、也走过了 USB 初始化**，然后才停止输出并最终被看门狗清掉。
所以“UART 没配对”和“ROM 没交接”两个解释都不成立；停在 `usb_cdc_console_init()`
之后到 shell 起来之间。

## 重要副作用：仪表会移动 bug

- 加 7 个阶段标记前：约 **1/4** 的轮次慢；加完后：**8/8 全慢**；再补 `[SCR2]`、
  再补 `reboot=` 标记，仍然 8/8。
- 也就是这条路径对 **几微秒级的时序** 极其敏感。因此所有阶段标记都收进了
  `#if TOS_DIAG`（release 镜像的时间线和以前一致），`BOOT_PHASE` 在非 diag 下是空宏。
- 这条本身是线索：USB 初始化（PLL_USB、`tud_init`、枚举窗口）内部有多个“按次数
  换算成时间”的等待，改动前后的差别足以决定它是否撞上某个窗口。

## 已排除：JEDEC 探测的 XIP 关/开窗口

把 `plat_board_init()` 里的 `rp2350_flash_detect()` 直接短路（`g_flash_detected = 0`）
再测 8 轮：**7/8 仍然 34.02 s**。所以不是"探测把 XIP 关掉又打开"留下的状态。

## 关键矛盾（下一轮从这里接）

阶段标记说那次启动走到了 **17**（`plat_board_init()` 结束，说明 `[A]/[B]/[U]` 和
TinyOS banner 都该打印过了），可是那 33.6 秒里线上 **一个字节都没有**。两者只能同时
成立于一种情况：**CPU 在跑，但 UART 的发送端没有时钟**——`plat_console_putc` 把字节
写进 DR，FIFO 却从不排空，于是全部丢弃，而代码继续往下走，最后卡在后面某处
（`ramdisk_init()` 一带，`banner()` 之前）。

这与第 12 阶段快照并不矛盾：`CLK_PERI_CTRL=0x10000800` 只证明"写进去的是这个值"，
不证明 clk_peri 真的在跑。下一轮要抓的是：

- `CLK_PERI_SELECTED`（0x50）与 `CLK_SYS_SELECTED`（0x44）在阶段 12 的实际值；
- 阶段 12/16 各读一次 `RP_UART_FR`（TXFE/BUSY），看 FIFO 有没有在排空；
- 若确认 clk_peri 没跑：`CLOCKS_CLK_PERI_CTRL` 的 AUXSRC 字段在 RP2350 上是否真如
  注释所说 0 = clk_sys（0x10000800 的 bit28 至今没解释）。

## 试过但没结论的

- `WATCHDOG_SCRATCH(7)` 想记录“ROM 重启返回了没有”（写 `"RBOO"`/`"FAIL"`），
  结果每次启动读回 **`00000004`**：**ROM 自己在用 scratch 4..7**（它会在把镜像
  交出去之前写），所以 6/7 不能用来自记。3 号没被覆盖，可以继续用。
- 想用 CDC 当第二见证：`reboot_latency.py` 的 CDC 侧重开逻辑有 bug（全 `NEVER`），
  尚未修；闸门自己的 CDC 腿是好的（同一轮里 `cdc=COM27` 能开）。
- ARM 镜像 10/10 在 0.38 s 回来，所以这不是 ROM 的通用行为，是 RV32 端口特有
  （或由 RV32 侧 `usb_cdc_init()` 的写法触发）。

## 下一步（有序）

1. 修 `reboot_latency.py` 的 CDC 重开（它现在把 `cdc` 置空后只在 `cdc_ports()` 里找，
   而 Windows 重枚举有几秒空窗），拿到“两个通道都在 34 s 后回来”的双见证。
2. 在 `usb_cdc_console_init()` 内部再分 4 段打标记（`usb_clock_init` / `pads` /
   `tud_init` / `boot_pump` 前后），把范围从“USB init 之后”缩到具体一句。
3. 对照实验：把 RV32 的 `usb_cdc_boot_pump()` 预算临时设为 0（不改别的），看停顿
   是否消失——这是最可能的嫌疑（它按 `plat_cpu_hz()` 换算次数，而 RV32 上
   `usb_cdc_poll()` 一次 2.66 µs，和 ARM 差一个量级，见 OPEN-ITEMS 第 4 条）。
4. 若确认在 pump，把它改成“按时间片”而不是“按次数”，顺带解决 #29。
