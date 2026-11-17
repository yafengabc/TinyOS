# B-4b：一份 ST7789 驱动供两个 RP2350 ISA（2026-11-17）

```text
commit:            fe1dc85
change:            drivers/display/st7789/{rp2350,rp2350rv} 两份合并为
                   drivers/display/st7789/lcd_st7789.c（+ 单份 font5x7.h）；
                   两块板 board.conf 指向共享目录
baseline:          5d1295f
target/config:     rp2350smp + rp2350rvsmp（对照产物）；12 目标 + fs_ro；QEMU 4
board/port/image:  无真机——但两个内核 ELF 与合并前**逐字节相同**，见下
toolchain:         go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（skip 4，均为 build/user/bin 指向别的目标，与改动前同一情况）
T1:                PASS 12/12 + configs/rv32-fsro.conf
T2:                PASS（smoke 4 目标；soak 3/3）
T3:                NOT RUN——不需要：镜像字节未变
T4:                NOT RUN
```

## 合并的是什么

两个端口去掉注释后的代码 371 / 367 行，`diff` 只有三处真差异：

| 差异 | ARM | RV32 |
|---|---|---|
| memmap 头 | `arm_memmap.h` | `rp2350rv_memmap.h` |
| 屏障 | `dmb` | `fence rw, rw` |
| tick 前的毫秒测量 | SysTick 下计数器 | TIMER0 自由计数 |

其余——引脚、SPI、DMA 通道 10、CS/DC、背光极性、像素层、字模——同一块硅，只是 ISA 不同。
现在这三处用 `#if defined(__riscv)` 就地选择，文件只剩一份，`font5x7.h` 也只剩一份。

## 等价性：不是论证，是字节

| 产物 | 合并前 | 合并后 |
|---|---|---|
| `rp2350_lcd_st7789.o`（ARM） | 13480 B，`662bb753…` | 13480 B，`662bb753…` |
| `rp2350rv_lcd_st7789.o`（RV32） | 17800 B，`1a09549d…` | 17800 B，`1a09549d…` |
| `kernel-rp2350smp.elf` | `7dfe15b1…` | `7dfe15b1…` |
| `kernel-rp2350rvsmp.elf` | `2ddd031a…` | `2ddd031a…` |

两个 ELF 整体 sha256 相同，所以 `.uf2` 也相同：**#15 目视确认过的那块屏，刷的就是这些字节**。
这就是为什么本条不需要再上板——不是"看着差不多"，是同一个二进制。

## 防回退

`internal/build/lcdproto_test.go` 换成了新形状的守卫：

- 只有一份驱动、一份 `font5x7.h`、一份 vendor init 表；
- `drivers/display/st7789/` 下不许再出现按 ISA 分的子目录；
- 两块 `board.conf` 必须指向共享目录，且不许残留 `st7789/rp2350` 路径；
- 三个 `#if defined(__riscv)` 位置必须还在——否则"合并"就退化成"只保留了一个端口"。

## 没做

- 没趁机改面板参数、DMA 通道或像素格式：这一轮只做合并，等价性是它唯一的卖点。
- 没在 QEMU 里"看"显示：QEMU 不跑这块屏，`lcd_proto` 的字节序列由 mock 测试覆盖。
