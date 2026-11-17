# 板组合（`base =`）与 `board/pico2`（#7 B1/B2，2026-11-17）

```text
commit:            b6be508
change:            board.conf 新增 base =；variant 目录同名文件遮蔽 base；
                   board/rp2350 的 LED/按键改为读设备表；新增 board/pico2 与
                   rp2350pico2 目标；TinyUSB 与 UF2 选择改为按 chip
baseline:          f7c4594
target/config:     rp2350pico2（新）+ 12 个既有目标 + configs/rv32-fsro.conf
board/port/image:  Pico 2 **未上板**；产物 build/rp2350pico2.uf2 1,769,984 B
toolchain:         go 1.27.3, clang 23.1.4, qemu 11.1.3, python 3.14.9
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（build/vet/test；boardconf 与 target 测试覆盖新板）
T1:                PASS 13/13（含 rp2350pico2）+ fs_ro
T2:                PASS（smoke 4/4）
T3:                NOT RUN（Pico 2 未接；ATK 的 LED/按键实现已改，需要一次冒烟）
T4:                NOT RUN
```

## B1 判据核对

| 判据 | 结果 |
|---|---|
| 同一 chip 下两个 variant 的差异从 board 层派生 | `board/pico2/board.conf`（flash、外设、驱动、[user] exclude）+ `board_data.c`（LED 引脚与极性、无屏/无卡/无按键） |
| 内核源码零改动 | `kernel/` 无新增文件、无 `BOARD_PICO2` 之类 `#ifdef` |
| chip 层零改动 | `Chips` 未动；`boardChips` 只加一行 `pico2: {rp2350}` |
| 不复制 `platform.c` | `board/pico2/` 只有 `board.conf` 与 `board_data.c` |
| 生成头往返 | `TOS_BOARD_NAME "pico2"`、`TOS_VARIANT_NAME "pico2"`、`TOS_FLASH_BYTES 0x400000` 等由 build.conf 生成，`board_config_check()` 仍会比对 |

## 机制

```ini
[board]
base = rp2350
```

- 源码：`board/rp2350/*.c` 全部编译；`board/pico2/` 里**同名文件遮蔽**（这里只有
  `board_data.c`）；
- include：`board/pico2` → `board/rp2350` → `board` → `arch/arm`，所以 pico2 没有自己的
  `board.h` 时用的是 rp2350 的，而不是 arch 的；
- 板级文件读取（`layout.c`、`kernel.ld`）走同一套 fallback（`Builder.boardFile`）。

## 顺带拆掉的硬编码

| 位置 | 之前 | 现在 |
|---|---|---|
| TinyUSB include | 板名列表 `rp2040/rp2350/rp2350rv` | chip 是 rp2040/rp2350 |
| UF2 打包 | `switch Target.Board` | 仍按板名，但 `rp2350, pico2` 共用一条（下一步应改为按 chip） |

## ATK 侧的影响（需要一次冒烟）

`board/rp2350/board_devs.c` 从“常量 + 直接读写引脚”改为“查表 + 极性来自 flag”：

```text
rp2350smp.elf 197540 B（不变）  rp2350rvsmp.elf 143972 B（不变）
.text/.data 哈希变化（LED/按键实现与 board_check 新增比对）
```

判据：`heartbeat 6/6`、`gpio`/LED 命令仍能点亮/熄灭、按键（BOOT）仍能读到按下。

## 没做

- Pico 2 上板与 A 轨（`PC=0xeffffffe`）——本卡只给出可刷的镜像；
- `rp2350rv` 的 `board_devs.c` 没有跟着表驱动化（它的 `platform.c` 是 RISC-V，走不了
  `base =`），两份板文件暂时不对等；
- `pico2smp` 目标（一行的事，等 UP 能启动再说）。
