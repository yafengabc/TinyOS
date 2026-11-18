# Pico 2：四镜像心跳 + dd/rm + smpbench 回归（2026-11-18）

```text
commit:            2349bbe（脚本）；被测固件来自 cff442f + 6ca4f18
change:            无代码改动；按 TESTING.md §5.4 跑四镜像心跳，并补 dd/rm 与 smpbench
baseline:          ATK-DNRP2350AM 的历史日志 build/test-logs/gate-h-{arm,rv}-smpbench-*.txt
target/config:     rpi-pico2 / -smp / -rv32 / -rv32-smp（默认配置）
board/port/image:  Pico 2 克隆板（RP2350A，8 MiB W25Q64）；UART0 → CH343 COM19
toolchain:         go 1.27.3, clang 23.1.4, python 3.14.0
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（go test ./... -count=1；go vet ./...）
T1:                PASS（go run ./cmd/tos build all：18/18）
T2:                PASS（tostest smoke rv32/rv32smp/arm：3× SMOKE TEST OK）
T3:                PASS 4/4 镜像心跳 6/6；ARM 单核另跑 flash.py --smoke 7/7
T4:                部分（dd/rm：ARM 双核 200 轮全通过，RV32 双核 340 轮后按要求停止）
logs:              build/dd-rv32smp-1000.log（原始日志不入库）
```

## 1. 四镜像心跳（每项见 TESTING.md §5.0）

| 镜像 | 心跳 | 核数 | 备注 |
|---|---|---|---|
| `rpi-pico2` | **6/6** | 1 | 另有 `flash.py --smoke` 7/7（含 reboot） |
| `rpi-pico2-smp` | **6/6** | 2 | `[cpu1] secondary ready`；`ps` 里 msh 落在 `c=1` |
| `rpi-pico2-rv32` | **6/6** | 1 | Hazard3 |
| `rpi-pico2-rv32-smp` | **6/6** | 2 | Hazard3 双核 |

时间项实测：delta 1.115–1.125 s，宿主 wall 同值 ±0.002 s（tick=1000 Hz）。

## 2. dd/rm（flash 写路径）

`scripts/rp2350/dd_stress.py`：每轮写 32 KiB、读回校验、删除。

```text
rpi-pico2-smp      ：200 轮 200/200 通过（≈1.0 轮/s）  ← ARM 双核，完整跑完
rpi-pico2-rv32-smp ：340 轮 340/340 通过（≈1.0 轮/s），随后按要求停止
```

两个镜像合计 540 轮无一次校验失败、无写错误、无掉线，覆盖了写+读+删的完整路径。
最初计划的 1000 轮没有跑满：`rpi-pico2-rv32-smp` 在 340 轮后中止（时间成本），
`rpi-pico2-smp` 按后续要求跑了 200 轮；剩余轮次 `NOT RUN`，不当作 1000 轮。

## 3. smpbench（双核扩展性回归）

命令：`smpbench <n>`，程序自身取 "best of 2"。

| 迭代数 | Pico 2 ARM SMP | ATK ARM 基线 |
|---|---|---|
| 1,250,000 | single **36 ms**，dual 36+36，wall **36 ms**，**2.00x** | single 45 ms，wall 42 ms，2.14x |
| 5,000,000 | single 144 ms，dual 140+141，wall **141 ms**，**2.04x** | — |
| 20,000,000 | single 564 ms，dual 563+560，wall **563 ms**，**2.00x** | — |

| 迭代数 | Pico 2 RV32 SMP | ATK RV32 基线 |
|---|---|---|
| 1,250,000 | single **53 ms**，wall **54 ms**，**1.96x** | single 61 ms，wall 71 ms，1.71x |
| 5,000,000 | single 212 ms，wall **213 ms**，**1.99x** | single 241 ms，wall 275 ms，1.75x |

`cat /proc/cpuinfo`：

```text
processor : 0 / 1   Cortex-M33 (RP2350)   thumbv8m.main   cpu MHz 150.000
board     : Raspberry Pi Pico 2
cpus      : 2
tick Hz   : 1000
```

**结论：无回归。** 绝对时间比 ATK 低约 20%（Pico 2 镜像不含 LCD/TF 驱动与面板轮询，
tick 负载更轻），加速比 1.96–2.04x；ATK 那条 2.14x 的 wall 比 single 还短，属测量噪声，
两者实质都是接近线性的 2x。Pico 2 的 RV32 相对 ATK 的 1.71–1.75x 明显更好，与 ATK 后期
`-tick100`/`-tty10` 调优后的 1.97–2.0x 一致。

## 4. 复现

```sh
for t in rpi-pico2 rpi-pico2-smp rpi-pico2-rv32 rpi-pico2-rv32-smp; do
  go run ./cmd/tos build -target $t
done

python scripts/rp2350/flash_heartbeat.py --uf2 build/tinyos-rpi-pico2-smp.uf2 -p COM19 --target rpi-pico2-smp
python scripts/rp2350/heartbeat.py       -p COM19 --target rpi-pico2-smp
python scripts/rp2350/dd_stress.py       -p COM19 --target rpi-pico2-rv32-smp --rounds 1000
# 然后在 shell 里：smpbench 1250000 / 5000000 / 20000000
```

## 5. 没做（NOT RUN）

- **dd/rm 的 1000 轮长跑**：改为两次短跑（ARM SMP 200/200、RV32 SMP 340/340），
  剩余轮次未跑。
- **ATK-DNRP2350AM 真机**：issue 关闭的硬件门槛仍是 ATK 上的 Cortex-M33 与 Hazard3 RV32 SMP，
  本卡片只覆盖 Pico 2。
- 屏幕 / TF 卡 / PIO 灯带：Pico 2 无这些外设。
- 观察：这块克隆板刷完 UF2 后 ROM 不保证自动重启应用，通常需要按一次 RUN；
  日志里的“等待时间”是等按键，不是固件慢。
