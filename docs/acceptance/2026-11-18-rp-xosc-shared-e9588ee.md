# 复测上游 HEAD：共享 XOSC 启动实现（2026-11-18）

```text
commit:            e9588ee（被测 HEAD）；a71f1d9 + e9588ee 为本次上游改动
change:            上游把 XOSC 启动收敛为按芯片族一份实现：board/rp2350-generic/rp2350_xosc.h
                   供 ARM/RV32 共用，RP2040 侧同样规则；去掉"双份等待"
baseline:          101eb12
target/config:     rpi-pico2-rv32-smp（冷启动实测）；全 18 目标构建
board/port/image:  Pico 2 克隆板（RP2350A，8 MiB W25Q64）；UART0 → CH343 COM19
toolchain:         go 1.27.3, clang 23.1.4, python 3.14.0
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（gofmt / go vet ./... / go test ./... -count=1）
T1:                PASS（go run ./cmd/tos build all：18/18）
T2:                PASS（tostest smoke rv32 / rv32smp / arm：3× SMOKE TEST OK）
T3:                PASS（Pico 2 rv32-smp：10 次拔插电源全部进 shell）
T4:                NOT RUN
logs:              用户终端观测
```

## 审阅（替换了本地实现，逐条核对）

`rp2350_xosc_start()` 的三条路径与实际情形一致：

| 情形 | 行为 |
|---|---|
| `XOSC_STATUS.ENABLED == 0`（冷启动） | 先写 `STARTUP.DELAY = 282`（12 MHz × 默认 multiplier 6 ≈ 6 ms），再使能，然后等 `STABLE`（硬件自己等满启动时间） |
| 已使能、未 STABLE | 等 `STABLE`（有界） |
| 已 STABLE（ROM 用 `DELAY = 0` 起的） | 再补一个启动周期再返回 |

- 全部等待有界，失败时返回 0，调用方留在 ROM 的时钟上（12 MHz / ROSC），不会死等；
- `XOSC_BASE`/寄存器定义移进共享头，ARM 与 RV32 不再各有一份，避免再次漂移；
- RP2040 侧（`board/rpi-pico/platform.c`）用同样的规则与常量，但它属于另一颗芯片、另一套 memmap，故保留本文件内的实现。

## 结果

- T0/T1/T2 全绿；
- Pico 2 `rv32-smp`：修复前约 1/2 冷启动失败（`XOSC.STARTUP` 复位值为 0，STABLE 只反映幅度）；
  共享实现这版 **10 次拔插全部进 shell**，与本地两版（`1628fc5` 稳定期、`a34c783` 写 STARTUP）行为一致。

## 没做（NOT RUN）

- **ATK 真机 `NOT RUN`**：改动作用于共享代码与 RP2040 端口，ATK 的两个 SMP 镜像需要补跑
  冷启动 + `accept33`。
- **RP2040 真机 `NOT RUN`**：按验收政策它不是关闭依据；本轮只做了构建。
- T4 长跑。
