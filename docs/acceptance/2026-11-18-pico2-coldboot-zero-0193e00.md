# Pico 2 冷启动清零：SDK 顺序 + 20 ms 起振（2026-11-18）

```text
commit:            0193e00
change:            clk_sys 切换改为 pico-sdk 的 clock_configure_internal() 顺序；
                   XOSC 起振延迟 6 ms -> 20 ms（RP2040 同步）；[U] cfg 不再重复打印
baseline:          6ccc346
target/config:     rpi-pico2-rv32-smp / rpi-pico2-smp（实测）；全 18 目标构建
board/port/image:  Pico 2 克隆板（RP2350A，8 MiB W25Q64）；UART0 → CH343 COM19
toolchain:         go 1.27.3, clang 23.1.4, python 3.14.0
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（gofmt / go vet ./... / go test ./... -count=1）
T1:                PASS（go run ./cmd/tos build all：18/18）
T2:                PASS（tostest smoke rv32 / rv32smp / arm：3× SMOKE TEST OK）
T3:                PASS（Pico 2 两个 SMP 镜像拔插电源：rv32-smp 20/20、arm-smp 20+/20）
T4:                NOT RUN
logs:              用户终端观测
```

## 三处改动

### 1. 切 clk_sys 按 pico-sdk 的顺序

```c
// pico-sdk clock_configure_internal()
// If switching a glitchless slice (ref or sys) to an aux source, switch
// away from aux *first* to avoid passing glitches when changing aux mux.
if (has_glitchless_mux(clock) && src == AUXSRC) {
    hw_clear_bits(&ctrl, SRC_BITS);          // 先回 clk_ref
    while (!(selected & 1u)) tight_loop_contents();
}
... 设 AUXSRC ...
... 再切 glitchless mux，等 SELECTED ...
```

原来 ARM 端一次写 `CTRL = SRC_AUX | AUXSRC(PLL)`（RV32 端在回 `clk_ref` 之后也有同样的合并写），
正是 SDK 注释里警告会传毛刺的写法。现在两端都按"回 clk_ref 等 SELECTED → 设 AUXSRC → 切 SRC"。

### 2. XOSC 起振 6 ms → 20 ms

pico-sdk 默认 multiplier 6，即 12 MHz 下 47×6 = 282 个 256 周期 ≈ 6 ms；实测在这块克隆板上
6 ms 仍有约 1/10 冷启动失败。SDK 把这个 multiplier 定义为"给起振慢的晶振留余量"的旋钮，
因此取 20（47×20 = 940 ≈ 20 ms）。"ROM 已使能过、DELAY=0"那条路径的补偿等待同步放大；
RP2040 侧同一常量一起改。

### 3. `[U] cfg=` 不再重复

启动时 `[E] usb hw=... cfg=...` 已经报过状态，tick 里的变化通知会在下一行重复一次。
现在 `plat_console_init()` 用刚报过的状态播种追踪器，只有之后 CDC 状态真的变化（拔插线）
才打印。

## 结果

| 镜像 | 拔插电源（真冷启动） |
|---|---|
| `rpi-pico2-rv32-smp` | **20/20** |
| `rpi-pico2-smp` | **20+/20** |

修复前两块镜像都约 1/2 失败（零输出、UART 与 USB 都无）。

## 没做（NOT RUN）

- **ATK 真机 `NOT RUN`**：改动作用于共享代码与 RP2040 端口，ATK 两个 SMP 镜像需补跑
  冷启动 + `accept33`。
- **RP2040 真机 `NOT RUN`**：只构建；按政策不作为关闭依据。
- T4 长跑。
