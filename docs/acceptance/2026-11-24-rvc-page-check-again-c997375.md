# 跨页检查回到按页：grain 捷径放过了 rcc（2026-11-24）

```text
TinyOS commit:     c997375
target/config:     5 个 Hazard3 构建 + rv32/rv32smp/arm/x86 QEMU + C3（COM4）
board/port:        ESP32-C3（COM4）；RP2350 / RP2040 真机 NOT RUN
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
T0:                PASS
T1:                PASS — 18/18
T2:                PASS — smoke 4/4；soak 3/3
T3:                PASS — C3 accept 42/42；RP2350/RP2040 NOT RUN
T4:                NOT RUN
```

## 上一版错在哪

上一版把“跨界”判断收紧成“只在 window grain 的倍数上”，理由是 `pmp_cover()` 的块至少
一个 grain。**两处都错**：

1. `pmp_cover()` 在第一个 grain 块之后可以发更小的块（28 KiB = 16 + 8 + 4），所以**内部
   边界不一定是 grain 的倍数**；
2. 真正咬人的是 **RX run 的结尾**：`wx_start` = 第一个可写段的 vaddr（`kernel/elf.c`
   的 `elf_probe`，原样返回，不做页圆整）。rcc 的 code run 是 `0x7000`（28 KiB），
   板上崩点 `0x20026ffe` 正是 28 KiB−2 —— 不是 16 KiB 的倍数。

结果就是**唯一有真机证据的 rcc 被放过了**，再上板会以同样的地址崩。

## 现在的规则：按页

`instructionStraddle()` 回到“任何 4 KiB 页边界”。为什么静态检查用页粒度是**精确集合的
上界**而不是乱猜：映射能产生的每个条目边界都是页对齐的——

- `pmp_cover()` 的块是 2^k 且 ≥ 1 页，基址是页对齐的窗口基址；
- 当 RX run 的尾巴不足一页时，`pmp_map()` 的阶梯退到 SNAP，它把 code/data 分界向下
  圆整到 **grain 的倍数**（也是页对齐）。

要拿到“精确集合”需要运行时窗口基址、会随 `sbrk` 移动的 `brk`、栈大小，以及整个
SPLIT→SNAP→IMAGE→WINDOW 降级阶梯——这些构建期都没有。所以按页查是“可能多说几个程序、
绝不会漏一个”的静态答案。

## 现状（rpi-pico2-rv32，16 KiB 粒度；ATK 同样）

```text
RVC bin/edit: rebuilt without compressed instructions
RVC bin/pio:  rebuilt without compressed instructions
RVC bin/sh:   rebuilt without compressed instructions
RVC bin/csc:  rebuilt without compressed instructions
RVC bin/rcc:  rebuilt without compressed instructions
RVC bin/rvm:  rebuilt without compressed instructions
```

- `rcc` 回来了（它的崩点在 RX run 结尾）。
- `msh` 的 code run 只有 4 KiB，没有内部页边界 → 保留 C，窗口 **16 KiB**。
- 内核保留 C。
- `pio`、`csc` 是被保守规则多换掉的（它们的跨界指令落在第一个 16 KiB 块内部）；要精确
  留下它们，需要把真实的 `pmp_map` 阶梯对所有可能的窗口基址跑一遍（现成的
  `internal/build/pmpmap_test.go` harness 可以扩展），属于另一量级的工作，本轮不做。

## 验证

T0 PASS；T1 18/18（五个 Hazard3 目标共 27 次按程序重建）；T2 smoke 4/4、soak 3/3；
C3 accept 42/42；`TestInstructionStraddle` 现在断言“跨 4 KiB 线的 32 位指令必须报”。

## 仍未做

RP2350 真机：`rpi-pico2-rv32-smp` 上重跑三条 CSC 链 + heartbeat，确认 rcc 的 no-RVC
镜像不再在 RX run 结尾崩。
