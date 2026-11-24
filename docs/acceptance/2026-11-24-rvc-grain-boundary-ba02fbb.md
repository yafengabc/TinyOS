# RVC 回退的边界是 grain，不是每一页（2026-11-24）

```text
TinyOS commit:     ba02fbb
target/config:     rpi-pico2-rv32 / -rv32-smp / rp2350rv-generic（16 KiB 粒度）；
                   atk-dnr2350am-rv32 / -smp（4 KiB 粒度）；rv32/rv32smp/arm/x86 QEMU
board/port:        ESP32-C3（COM4）；RP2350 / RP2040 真机 NOT RUN
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
T0:                PASS
T1:                PASS — 18/18
T2:                PASS — smoke 4/4；soak 3/3
T3:                PASS — C3 accept 42/42；RP2350/RP2040 NOT RUN
T4:                NOT RUN
```

## 规则

`pmp_cover()` 从 **grain 对齐**的窗口基址出发，块大小至少一个 grain，所以块边界只能落在
**grain 的倍数**上（不足一个 grain 的余块总是最后一块）。于是：

> **code run（`.text`+`.rodata` 到可写段起点、按页取整）装得下一个 grain 的程序，
> 没有任何内部边界，永远不会跨界。**

所以“text < 8K 不会有事”对 16 KiB 粒度的三块板成立；4 KiB 粒度的 ATK rv32 阈值是 4 KiB。
检查器现在按这条走（`instructionStraddle(code, base, grain)`，条件 `(addr % grain) + size > grain`），
不再把每一个 4 KiB 页都当边界。

## 效果（`[user] rvc = auto`）

| 目标（grain） | 换 no-RVC 的程序 | 说明 |
|---|---|---|
| `rpi-pico2-rv32` / `-rv32-smp` / `rp2350rv-generic`（16 KiB） | **`csc`**（52 KiB） | 之前是 6 个；`edit`/`rvm`（8 KiB run）、`rcc`（32 KiB，唯一可能的边界在 16 KiB）、`sh`（16 KiB）都保留 C |
| `atk-dnr2350am-rv32` / `-smp`（4 KiB） | `edit pio sh csc rcc rvm` | 不变：每页都是边界 |

- `msh` 的 run 是 8 KiB，两种粒度下都保留 C → 窗口 **16 KiB**（不再有 20/32 KiB 的分支）。
- 内核保留 C（`march` 仍是 `rv32imac`）。
- 属性抽查（ATK 目标）：`msh.o` = `…c2p0…`；`noc/rcc.o` = 无 `c2p0`。

## 顺带：noc 树改成懒编译

之前 `auto` 会为**每个**程序编一份 no-RVC 对象（`build all` 约 275 次多余编译）。现在只在
链接检查之后、为真正跨界的程序（加公共运行库一份）编译：`build all` 的额外编译降到 **14**。

## 验证

- T0 PASS；T1 18/18；T2 smoke 4/4、soak 3/3；C3 accept 42/42。
- `TestInstructionStraddle` 新增 grain 用例：同一条 4 字节指令跨 4 KiB 线，在 16 KiB 粒度下
  不报、在 4 KiB 粒度下报；跨 16 KiB 线在 16 KiB 粒度下报。

## 仍未做

- RP2350 真机（本机无板）：需要在 `rpi-pico2-rv32-smp` 上重跑三条 CSC 链与 heartbeat，
  确认 `csc` 的 no-RVC 镜像在真实 PMP 下正常，其余程序带 C 也正常。
