# Pico 2 冷启动：晶振稳定期修复（2026-11-18）

```text
commit:            a34c783（机制改为 SDK 写法）+ 1628fc5（稳定期等待）
change:            XOSC 使能前写 STARTUP.DELAY = 282（≈6 ms，pico-sdk 默认）；STABLE 之后
                   保留稳定期等待（覆盖"ROM 已使能且 DELAY=0"的情况）；
                   升频前要求 clk_ref 确为晶振、并用 FC0 实测 PLL 输出
baseline:          1c09161
target/config:     rpi-pico2-rv32-smp（实测）；其余 17 目标构建
board/port/image:  Pico 2 克隆板（RP2350A，8 MiB W25Q64）；UART0 → CH343 COM19
toolchain:         go 1.27.3, clang 23.1.4, python 3.14.0
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（gofmt / go vet ./... / go test ./... -count=1）
T1:                PASS（go run ./cmd/tos build all：18/18）
T2:                PASS（tostest smoke rv32 / rv32smp / arm：3× SMOKE TEST OK）
T3:                PASS（Pico 2 rv32-smp：稳定期版 10/10、SDK 机制版同样通过；修复前约 1/2）
T4:                NOT RUN
logs:              用户终端观测（两轮各 10 次拔插），失败现场见下面第二条
```

## 现象（修复前）

拔插电源（真正的冷启动）约 **一半**失败，形态有两种：

1. **完全无输出**：UART 零字节，USB CDC 也不出现，板子看起来没上电。
2. **打了半行就 trap**（一次 RV32 现场）：

```text
[A] pre-pll ok
*** RV32 TRAP ***
load access fault: mcause=0x5 mepc=0x1000e59a mtval=0x0
ra=0x00000000 sp=0x20016b00 s0=0x20016b80 s1=0x40070000 a0=0x00000000 a1=0x0001869f
```

`mepc` 反查到 `plat_board_early+0x30e`，正是打印 `[A]` 那行字符串的循环里的
`lbu a0, 0(a0)`——**代码能取指，第一次 flash 数据读就 fault**。
（`a1=0x1869f` 是同一个 `plat_console_putc` 里的 100000 次自旋上界，`s1=0x40070000` 是 UART0 基址，说明现场就在这行。）

## 根因

`clocks_init()` 一看到 `XOSC_STATUS.STABLE` 就把 `clk_ref` 从 ROSC 切到晶振；
而此时 `clk_sys` 还挂在 `clk_ref` 上，所以**切 clk_ref 等于切 clk_sys**。

- `STABLE` 是**幅度**阈值，不是"频率已稳"：刚起振的晶体还会漂几毫秒；
- QMI 的采样延迟按 **clk_sys 周期**计，切换落在瞬态上 → 切换后第一次 flash
  **数据读**采样在错误时刻 → load access fault；
- 已经进入流水线的取指不受影响，所以有的启动能打半行；而 fault handler 自己
  也在 flash 里，于是二次 fault → 完全静默（对应"零输出"那种形态）。

pico-sdk 在同一位置就是这样处理的：`PICO_XOSC_STARTUP_DELAY_MULTIPLIER` 毫秒的
启动延迟，然后才用晶振。

## 修复（两版，最终按 pico-sdk 的机制）

pico-sdk 不是"STABLE 之后再等"，而是**在使能之前写 `XOSC.STARTUP.DELAY`**：

```c
// xosc.c
#define STARTUP_DELAY ((((XOSC_HZ / KHZ) + 128) / 256) * PICO_XOSC_STARTUP_DELAY_MULTIPLIER)  // 默认 6
xosc_hw->startup = STARTUP_DELAY;   // 12 MHz: 47 * 6 = 282 → 282*256/12MHz ≈ 6 ms
hw_set_bits(&xosc_hw->ctrl, ENABLE);
while (!(xosc_hw->status & STABLE)) tight_loop_contents();
```

而 `XOSC_STARTUP` 的复位值是 **0**（`XOSC_STARTUP_RESET = 0`），我们从来没写过它，
等于让 XOSC "一有幅度就报 STABLE"。

- `1628fc5`：先加"STABLE 之后再等 ~6 ms"的稳定期（与 SDK 的 6 ms 等量），实测 10/10；
- `a34c783`：改成 SDK 原样，使能前写 `STARTUP.DELAY = 282`，让 STABLE 本身就要等；
  稳定期等待保留，用于"ROM 已使能过晶振、DELAY 已是 0、无法追溯"的情况。

保留的两道检查（同一类错误：相信状态位而不是测量）：

- `pll_sys_init()` 在 `clk_ref` 不是晶振时**直接返回**——分频参数是 12 MHz 晶振的，
  在 ROSC(~6.5 MHz) 上只会出 ~81 MHz，flash 时序不是按它配的；
- 升频前用 FC0 实测 PLL 输出（149–151 MHz），不合格就留在 `clk_ref` 上启动。

## 复现

```sh
go run ./cmd/tos build -target rpi-pico2-rv32-smp
# BOOTSEL 拖入 build/tinyos-rpi-pico2-rv32-smp.uf2，然后反复拔插电源
# 修复前：约 1/2 成功；修复后：10/10
```

## 没做（NOT RUN）

- **ATK 真机本轮 `NOT RUN`**：改动同样作用于 ATK 的 `clocks_init()`，但板子不在台上；
  接上后应补一次冷启动 + `accept33`。
- Pico 2 的 ARM 单核 / SMP 镜像冷启动未单独计数（同一段代码；ARM SMP 此前一轮
  9 次连续启动均成功，但那组里失败的启动不会被统计，不能当作通过证据）。
- T4 长跑。
