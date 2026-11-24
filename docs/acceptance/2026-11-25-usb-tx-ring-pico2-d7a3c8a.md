# USB 控制台 TX 环（`694945a`）在 Pico 2 上的板级验证：长输出仍会截断（2026-11-25）

```text
TinyOS commit:     d7a3c8a（被测镜像）；TX 环本身来自 694945a
target/config:     rpi-pico2 / -smp / -rv32 / -rv32-smp（Pico 2，RP2350）
board/port:        Pico 2 —— UART0 COM19、USB CDC COM27
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
T0:                NOT RUN
T1:                4/4 PASS
T2:                NOT RUN
T3:                FAIL —— 待跑清单 (b) 不满足；(a) 的 N/U 达标，D 不达标
T4:                NOT RUN
```

## 验证方法

按 `docs/OPEN-ITEMS.md` §5c 的要求全部走 **CDC 控制台（COM27）**，UART0 只用来读
计数器（原因见下）。载荷是 21 200 B 的 ASCII 文本（400 行，`line 000` … `line 399`），
用 `tosput` 上传后 `cat` 整份，主机侧逐字节比对；同一条命令在 UART0 上作对照。

## 结果

| 镜像 | 控制台驱动 | `cat` 21 200 B | 实际收到 |
|---|---|---|---|
| `rpi-pico2` | `usb/rp2350`（新环） | 0/3 完整 | 255–271 B |
| `rpi-pico2-smp` | `usb/rp2350`（新环） | 3/6 完整 | 失败时 276 B |
| `rpi-pico2-rv32` | `usb/rp2350rv`（**未改**） | 0/3 完整 | 1 216 B |
| `rpi-pico2-rv32-smp` | `usb/rp2350rv`（**未改**） | 3/3 完整 | 21 621 B |

UART0 对照：四条镜像都是完整 21 621 B，首尾行与提示符齐全 —— shell 与 `cat` 本身
没有问题，丢字节只在 CDC 镜像路径上。

## 计数器（ARM 两镜像）

`cat /proc/usbboot` 在 CDC 上本身就会被截断（330 B 的输出，最后一行 `console tx …`
在 6 次里丢了 4 次），所以数值从 UART0 读：

- 空载：`lock timeouts 0  ungated=0`，`console tx dropped=0  handed=30`。
- 3 次 `cat` 之后：`dropped=64 263  handed=1 813`。3 份文件共 ~64.8 KB，说明失败
  的运行里几乎整份被丢。
- SMP 6 次 `cat`（3 次失败）之后：`dropped=64 226  handed=66 500`，
  64 226 ≈ 3 × 21.2 KB，与“3 次全丢、3 次全成”完全对得上。

即 (a) 的 `N = 0`、`U = 0` 达标，但“主机在读”的前提下 `D` 并不为 0。

## 对照：改动之前（`0a30b69`，无 TX 环）

同一 `cat` 每次送出 576–581 B，也不完整。所以“单次大输出丢尾巴”不是 `694945a`
引入的；这轮把边界从 ~576 B 变成 256 B（新环的大小），**(b) 的目标没有达成**。

## 观察到的机理

`putc()` 入环满了就计数丢弃，泵在 tick 里每拍只搬几十字节。`cat` 一次 `write()`
如果远大于 256 B，环外的那部分就整段丢掉，连行尾和提示符一起丢。SMP 上出现
“3/6 成功、3/6 全丢”的双峰，说明成败取决于另一核是否恰好在写的过程中把环排空；
单核则没有任何并发排水，稳定失败。

## RV32 不在这次改动的范围内

RV32 镜像用的是 `drivers/usb/rp2350rv/usb_cdc.c`，`694945a` 只改了 `usb/rp2350` 与
`usb/rp2040`。所以 RV32 的 `console tx dropped/handed` 恒为 0 是**计数器不存在**，
不是“没丢”；RV32 单核的 1 216 B 截断是它自己的旧路径行为，RV32 SMP 完整则说明
那条老路径在双核下能排完。

## 结论

§5c 的 (b) 在 Pico 2 上 **NOT MET**：(ARM 单核 0/3、ARM SMP 3/6、RV32 单核 0/3 完整)。
赔偿不了的原因是“单次写大于环容量时有界丢弃”，按 §5b 第 5 条的边界这属于已知限制，
但它恰好是 (b) 想验的东西，因此不能按 PASS 收口。

下一步有两条互斥的路，需先确认 `cat` 的写粒度再选：

1. 若 `cat` 确实是“一次大 `write()`”，修 `console_write`/`cat` 让它分块并在块间
   让出（副作用最小，UART 行为不变）；
2. 若要保持任意大写都完整，就得让 `putc()` 在环满时阻塞或重试 —— 这条路会把
   “不在关中断路径上等锁”的收益吐回去一部分，需要单独评估。

## 未覆盖

- ATK（COM14/COM26）不在场，`atk-dnr2350am` / `-smp` 的同一项未测。
- RP2040 两镜像（`drivers/usb/rp2040` 同样改了 TX 环）未测，§5c (c) 仍未跑。
- `run prot heap` + `deep 3` 的组合只在 ARM 单核上跑过一次，`N` 仍为 0。
