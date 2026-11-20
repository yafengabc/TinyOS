# 2026-11-20 RV32 无等待镜像的人工冷启动 ×30（#29）

> **再更正（约 17:45）**：ATK-DNRP2350AM 接台后第一次无等待启动就复现了 `mcause=0x3`（`usbd_control_xfer_cb` 的 `TU_ASSERT`），等待已恢复、#29 已重开。本卡描述的是当时的结论，已被 `docs/acceptance/2026-11-20-rv32-atk-trap-reproduced.md` 推翻。

> 计数更正（17:25）：操作者没有数拔插次数，说“可能拔了 30 次”。本文一律以**监视器
> 记录的 `[A] pre-pll` banner** 为准（30 次上电），不再声称人工做了整数 20 次。

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
- 操作：人工拔插**给板子供电的 USB 线**，断电约 1 s、上电约 2 s；操作者未计数，
  实际次数以监视器看到的 banner 为准。

## 数据

原始日志：`build/cold-nowait.log`（监视器原始字节 + 事件行）。

| 项 | 结果 |
|---|---|
| 用户拔插循环 | 未计数（操作者自述“可能 30 次”） |
| `[A] pre-pll` banner | **30 次**（部分插拔产生两次复位，0.2–0.3 s 内连续两个 banner） |
| 走到 `TinyOS shell` + `tos> ` | **28 次** |
| `mcause=` / `ebreak` / access fault | **0 次** |
| 每次完成后 CDC 节点 | 约 0.4 s 内出现 |

两段没有 shell 的记录，逐字节检查过：

- 第 1 段是监视器启动时正好赶上刷机后的那次启动，banner 中途开始抓；
- 第 13 段只有 93 字节，紧接着 0.25 s 后就是下一次完整启动——是插拔瞬间的第二次
  复位，不是 trap，日志里没有任何 `mcause=`。

也就是说：**监视器记录 30 次上电，没有一个 trap，28 次完整进 shell。**
次数来自日志而不是人工计数（见文首更正）；#29 要求的“≥20 次冷启动”按
28 次完整启动判定已满足。无等待镜像的暖重启此前也已 20/20（0.18–0.19 s）。

## 边界

- 板子只有 **Pico 2 / RP2350 RV32 SMP**。11-19 触发 trap 的 **ATK-DNRP2350AM 没接在台上**，
  这块板没有冷启动数据。
- 因为没出 trap，没有 `mepc` 可用带符号 ELF 反查。

## 结论（17:28 用户选了方案 1，已执行）

用户决定接受 Pico 2 的证据：**删除 `usb_cdc_wait()` 并关闭 #29**。省下启动期
0.11 s（有主机）和无主机时的 8.3 s；ATK 板未验这一点写进 issue 的关闭说明，
以后若在 ATK 复现就开新 issue。

执行结果：等待函数及调用已删；仅为 #29 加的 `bootpump:` 仪表、`plat_boot_us()`/
`plat_usb_boot_pump()` 钩子、`[C] usbwait` 启动行一并撤掉（保留稳态 `usbpoll:`）。
`/proc/usbpoll` 现在只有 `usbpoll: ...` 一行。全目标构建与真机回归见关闭提交。

本卡里的 `bootpump: 4805 ns/poll, 72836/...` 是**删除之前**那版固件读到的数，
作为历史证据保留。

**删除后的最终回归**：`go build/vet/test ./...` 全绿、`tos build all` 18/18；
Pico 2 刷入最终镜像后 `cat /proc/usbpoll` 只剩稳态一行
（`usbpoll: 2606 ns/poll`），`uptime`、`echo` 正常；
`reboot_soak.py -n 20` **20/20 PASS**（prompt 0.18–0.19 s、uptime 0.08 s，
csv `build/final-nowait-reboot.csv`）。
