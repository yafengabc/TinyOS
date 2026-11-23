# 用户 ISA 归 target conf（Hazard3 五个目标 `march = rv32ima`）（2026-11-24）

```text
TinyOS commit:     252af6b
target/config:     rpi-pico2-rv32 / -rv32-smp / atk-dnr2350am-rv32 / -smp /
                   rp2350rv-generic（构建门禁）；rv32 / rv32smp / arm / x86 QEMU；18 目标
board/port:        ESP32-C3（COM4）；RP2350 / RP2040 真机 NOT RUN（不在本机）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
host:              Windows 11 / EKSOFT
T0:                PASS — go test -count=1 ./...
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — smoke rv32/rv32smp/arm/x86；soak 3/3；rvm-cover 3/3（rv32、arm）
T3:                PASS — C3 accept.py 42/42；RP2350 / RP2040 NOT RUN
T4:                NOT RUN
```

## 背景

上一版把“用户代码不要 RVC”先写成 Go 里的 `chip == "rp2350"`，再改成
`board.conf` 的 `[user] rvc = 0`。两条都是给 build system 加板级知识：每来一块
“一段 code 被拆成多个 PMP 条目”的板子，就要动一次构建器（或学会一个新键）。

## 改法：ISA 本来就是 target 的事实

五个 Hazard3 目标的 `targets/*.conf` 从 `march = rv32imac` 改成 `march = rv32ima`
（原因写在行旁边）。构建器不再有 `withoutRVC()`、`-DTOS_USER_NO_RVC`、`[user] rvc`
这些东西：用户代码和内核都按 target 的 `march` 编译。新加同类板子只改它自己的
target conf。

`checkCodePages()` 的门限也改成同一个事实：rv32 且 `march` 里没有 `c` 的目标才检查
（五个 Hazard3 + `rv32nc`），TOR 端口带 RVC、不检查。

`user/msh` 的窗口承诺改用编译器的 `__riscv_compressed` 判断（16 KiB 有 RVC；
20 KiB / 16 KiB 粒度上 32 KiB 无 RVC），不再依赖构建期宏。

## 代价（实测）

| 项 | 数字 |
|---|---|
| 用户代码 | +26% 合计：`csc` 47 900→58 784、`rcc` 31 098→39 432、`msh` 4 016→5 420、`hello` 464→684 |
| 内核 | 也去 RVC，但跑在 XIP flash（0x10000000）：.text+.rodata **164 032 B（只花 flash）**，RAM 端只差 **576 B** |
| msh 窗口 | Hazard3 上 20 KiB（16 KiB 粒度板分配成 32 KiB）；C3/ARM/QEMU 仍 16 KiB |

## 验证

属性（`readelf` 语义）：

```text
rpi-pico2-rv32-smp  user msh.o            rv32i2p1_m2p0_a2p1_...（无 c2p0）
                    kernel rv32_platform.o rv32i2p1_m2p0_a2p1_...（也去 RVC）
esp32c3-generic     user + kernel          ...c2p0...（不变）
rv32 (QEMU)         user + kernel          ...c2p0...（不变）
rpi-pico2 (ARM)     非 RISC-V，不适用
```

负例（门禁仍有效）：`user/rcc/opt` 里放 `-march=rv32imac`（opt 追加在最后，会覆盖
target 的 march）→ 构建停：

```text
tos: bin/rcc: 4-byte instruction at 0x00000ffe crosses a 4 KiB page boundary;
Hazard3 splits the code run at page granularity ...
```

其余：T0/T1/T2 全绿；`rvm-cover.py` 在 rv32 与 arm 上三条链全过；C3 accept 42/42。

## 没做的（下一步候选）

**按程序自动回退**：先用 `+c` 编译并链接，跑 `checkCodePages()`，只有踩到页边界的
程序（和公共运行库的那一份）用 no-RVC 重编。收益：按 QEMU 同一份源码的扫描，
`msh` 本身不跨界，能保住 16 KiB 窗口；约 41/50 个程序保留 RVC，能省回 ~18 KiB。
代价：公共对象要有第二份变体、每个回退程序多一遍编译+链接、程序 ISA 变成“看布局”
（小改动可能翻转），构建日志必须打印 `RVC n / no-RVC m`。当前没做，等窗口/flash
真成为约束再说。
