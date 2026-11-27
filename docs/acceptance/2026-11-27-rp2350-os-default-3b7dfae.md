# RP2350 硬件目标默认 `-Os`（用户程序 `-O2`，2026-11-27）

```text
commit:            3b7dfae（targets）+ 59c877e（文档）
baseline:          a626381（user_opt 旋钮）
change:            八个 RP2350 硬件目标加 opt = -Os、user_opt = -O2：
                   atk-dnr2350am{,-smp,-rv32,-rv32-smp}、
                   rpi-pico2{,-smp,-rv32,-rv32-smp}
board/port/image:  ATK-DNRP2350AM / Pico 2（板测数据来自板侧隔离实验）
toolchain:         go 1.27.3；clang/ld.lld 23.1.4
host:              Windows / 本机
T0:                PASS（go test -count=1 ./...；go vet ./...）
T1:                PASS 18/18
T2:                PASS（QEMU 目标未改，smoke 仍按 -O2）
T3:                PASS（板侧隔离实验：内核 -Os + 用户 -O2 的镜像进 shell、CoreMark 487、
                   smpbench 2.00–2.05x；本次只把同一组合设为 target 默认）
T4:                NOT RUN（30 分钟矩阵未在这个 opt 档重跑）
logs:              build/t-osadopt-*.log、build/t-os-rel.log（不入库）
notes:             证据卡 docs/acceptance/2026-11-27-user-opt-knob-a626381.md 的“板上隔离测量”
```

## 板侧数据（用户程序逐字节相同，三版都 `user_opt=-O2`）

| 内核 | `.text`（默认） | CoreMark | smpbench 均值 |
|---|---|---|---|
| `-O2` | 144,000 | 478 | 1.95x |
| **`-Os`** | **100,670** | **487** | **2.02x** |
| `-Oz` | 88,128 | 476 | 1.87x |

`-Os` 比 `-O2` 小 30%、单线程还快 1.9%、SMP 最好；`-Oz` 再小 12 KiB，但 SMP 扩展性
掉到 1.85–1.90，低于 1.9 的稳定线（1.8 的硬线能过，但余量小）。

## 本次构建的尺寸

| 镜像（默认配置） | `.text` | 之前（-O2 + noinline） |
|---|---|---|
| `atk-dnr2350am-smp` | 100,682 | 144,012 |
| `atk-dnr2350am-rv32-smp` | 102,960 | ~123,966 |
| `rpi-pico2-smp` | 96,574 | 137,672 |
| `rpi-pico2-rv32-smp` | 98,596 | ~123,000 |

release（`configs/release.conf`，ATK M33）：**92,998 B** 文本（-O2 是 134,612）。

## 没有改的（有意）

- `esp32c3-generic`：保持 `opt = -Oz`（C3 accept 42/42，用户程序也 -Oz 是有意的）。
- QEMU 目标（rv32/rv32smp/rv32nc/arm/x86）：保持 `-O2`——size 优化级别会让 TCG
  逐指令重翻译，冒烟都过不去。
- `rp2350-generic` / `rp2350rv-generic`：两个芯片基线保持 `-O2`，bring-up 用。
- RP2040（`rpi-pico*`）：没有 M0+ 的 -Os 测量，暂不动。

## 未跑

- **T4**：这个 opt 档没有重跑 30 分钟矩阵；`-Os` 只改了优化级别，行为与 `-O2` 相同，
  但按规矩要把 `rpi-pico2*` 四镜像的 T4 重跑一遍（#60–#63 已关，可另开或复用卡的
  “after opt change”一节）。
- **RP2040 的 -Os**：未测，保持 -O2。
