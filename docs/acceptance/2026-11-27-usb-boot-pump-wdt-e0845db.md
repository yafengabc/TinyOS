# USB 预 tick 等待期间喂看门狗（`-Oz` 启动复位，2026-11-27）

```text
commit:            e0845db
change:            新增 plat_watchdog_feed()（弱默认空；RP2350 ARM/RV32 转给 wdt_feed）；
                   ARM 的 usb_cdc_boot_pump() 每 4096 次轮询喂一次、usb_cdc_init() 后
                   再喂一次；RV32 的 USB 等待循环同样处理
baseline:          6769ca8
target/config:     18 目标；rv32 / x86 QEMU
board/port/image:  ATK-DNRP2350AM 不在本机（复验由板侧做）
toolchain:         go 1.27.3；clang/ld.lld 23.1.4
host:              Windows / 本机
T0:                PASS（go test -count=1 ./...）
T1:                PASS 18/18
T2:                PASS（rv32 / x86 smoke）
T3:                NOT RUN（ATK 上的 -Oz 复验）
T4:                NOT RUN
logs:              build/t-wdt-*.log（不入库）
notes:             #73 的第一版修复；如果仍复位，下一步查 [PREV] 与关狗对照
```

## 现象（板侧卡 `2026-11-27-kernel-opt-o2-vs-oz-9518609.md`）

- ATK M33（UP 与 SMP）：UART0 到 `[D] cpu0` + banner 后反复重启，没有 `[E] usb`，
  没有 CDC，没有 `tos>`。
- ATK RV32 SMP：过了 `[u4] pullup` / `[U] usb hw=1 cfg=1` / banner，UART0 静默；
  CDC 出现但没有 shell。

## 判断（M33）

复位点落在 `plat_console_init()` 里、`[E] usb` 之前。这一段：

- 看门狗已经在 `[B]` 由 `wdt_enable()` armed（`WATCHDOG_RELOAD 0x1fffff`，约 2.1 s）；
- 从 `[B]` 到第一个 tick 之间**没有任何地方喂狗**；
- `usb_cdc_boot_pump()` 的预算是按 -O2 代码标定的**轮询次数**（40 000 次 ≈ 250 ms），
  每轮是 `tud_task()`。`-Oz` 让 TinyUSB 慢几倍时，整段等待可以超过 2.1 s，板子被
  看门狗复位——正好是"反复重启、没有 `[E]`"。

RV32 的等待是按 TIMER0 的 400 ms 墙钟限的，所以不会超时，这也解释了为什么只有 M33
复位、RV32 是另一种现象（过了 USB、没有 shell，见 #73 的另一半）。

## 改动

- `kernel/plat/tasks.h`：`plat_watchdog_feed()` 契约；`kernel/main.c` 弱默认空。
- `board/rp2350-generic`、`board/rp2350rv-generic`：转给各自的 `wdt_feed()`。
- `drivers/usb/rp2350/usb_cdc.c`：boot pump 每 4096 次轮询喂一次，`usb_cdc_init()`
  返回后再喂一次；预算与"mounted 即停"的行为不变。
- `drivers/usb/rp2350rv/usb_cdc.c`：等待循环同样每 4096 次喂一次（双保险）。

## 验证

```text
go test -count=1 ./...     PASS
18/18 targets              PASS
smoke rv32 / x86           SMOKE TEST OK
```

## 没跑的 / 下一步

- **ATK `-Oz` 复验**：`NOT RUN`（无板）。板侧要重刷 M33 UP/SMP 的 `-Oz`，期望过
  `[E]` 并进 shell；`-O2` 不回归。
- **如果仍复位**：贴 `[PREV]`（`.noinit` 上次停点）+ 复位间隔秒数；再给一个临时
  关掉 `wdt_enable()` 的 `-Oz` 镜像（不进仓库）做对照——能起来就确认是看门狗。
- **RV32 的另一半**（CDC 在、无 shell）不在本提交里，用 `[PREV]`、`usb_pump=false`
  和 `usb_pump_fallback=true` 的对照定位（#73）。
