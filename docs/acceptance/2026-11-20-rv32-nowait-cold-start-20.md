# 2026-11-20 RV32 无等待镜像的 20 次人工冷启动（#29）

## 目的

上一张卡证明：在当前代码和 Pico 2 上，11-19 的紧泵挂死和 `mcause=0x3` trap 都不复现，
但那次只有「刷机首启 + 20 次 ROM 暖重启」。#29 要求删等待前必须过 **20 次冷启动**，
所以这次由人工反复拔插板子供电，前台只观察、不写板子。

## 方法

- 镜像：`TOS_KEEP_SYMS=1 go run ./cmd/tos build -target rpi-pico2-rv32-smp`，
  临时把 `usb_cdc_wait()` 改成直接 `return`（实验代码测完已撤）。
- 工具：`scripts/rp2350/cold_watch.py`，刷机后启动，持续打开 UART0（COM19），
  记录 `[A] pre-pll` banner、`mcause=`/`ebreak`、`TinyOS shell`、`tos> `、
  以及 CDC 节点（2e8a:000a）的出现/消失。UART0 是主见证：CDC 不枚举时它还能说话。
- 操作：人工拔插**给板子供电的 USB 线**，断电约 1 s、上电约 2 s，共 20 次。

## 数据

原始日志：`build/cold-nowait.log`（监视器原始字节 + 事件行）。

| 项 | 结果 |
|---|---|
| 用户拔插循环 | 20 次 |
| `[A] pre-pll` banner | **30 次**（部分插拔产生两次复位，0.2–0.3 s 内连续两个 banner） |
| 走到 `TinyOS shell` + `tos> ` | **28 次** |
| `mcause=` / `ebreak` / access fault | **0 次** |
| 每次完成后 CDC 节点 | 约 0.4 s 内出现 |

两段没有 shell 的记录，逐字节检查过：

- 第 1 段是监视器启动时正好赶上刷机后的那次启动，banner 中途开始抓；
- 第 13 段只有 93 字节，紧接着 0.25 s 后就是下一次完整启动——是插拔瞬间的第二次
  复位，不是 trap，日志里没有任何 `mcause=`。

也就是说：**20 次人工冷启动（30 次实际上电）没有一个 trap，28 次完整进 shell。**
无等待镜像的暖重启此前也已 20/20（0.18–0.19 s）。

## 边界

- 板子只有 **Pico 2 / RP2350 RV32 SMP**。11-19 触发 trap 的 **ATK-DNRP2350AM 没接在台上**，
  这块板没有冷启动数据。
- 因为没出 trap，没有 `mepc` 可用带符号 ELF 反查。

## 当前状态与待决

实验代码已全部撤回，板子已刷回正式镜像并验证：`/proc/usbpoll` =
`usbpoll: 2686 ns/poll`、`bootpump: 4805 ns/poll, 72836 of 1730769 polls in 349978 us`，
shell 正常。

证据现在支持「当前代码里等待可以去掉」，但覆盖只有 Pico 2。两个选择：

1. 接受 Pico 2 的证据：删除 `usb_cdc_wait()` 并关 #29（启动省 0.11 s，
   无主机时省 8.3 s）；ATK 板以后若复现再开新 issue。
2. 保守：等 ATK 板接上后补 20 次冷启动，再决定；在此之前等待保留。

在用户确认前**不改产品代码**，issue #29 保持打开。
