# 2026-11-20 #29 复现尝试：紧泵与去等待都没有复现（WONTFIX 证据链）

> **再更正（约 17:45）**：ATK-DNRP2350AM 接台后第一次无等待启动就复现了 `mcause=0x3`（`usbd_control_xfer_cb` 的 `TU_ASSERT`），等待已恢复、#29 已重开。本卡描述的是当时的结论，已被 `docs/acceptance/2026-11-20-rv32-atk-trap-reproduced.md` 推翻。

> 后续（同日 17:28）：用户选择接受 Pico 2 证据，`usb_cdc_wait()` 已删除、#29 已关。
> ATK 板未验的点写进关闭说明；若以后在 ATK 复现请开新 issue。


## 目的

11-19 有两个未解释的现象，都在 RV32/Hazard3：

1. 去掉 `usb_cdc_wait()` 后，shell 提示符之后 trap：`mcause=0x3`、`mepc=0x10013d0a`。
2. 把等待换成 ARM 的 40 000 次紧泵后，CDC 不枚举、UART 0 字节，只能按 BOOTSEL 刷回。

本次按 issue #29 的边界做**只改等待写法**的安全复现：`TOS_KEEP_SYMS=1` 构建带符号
镜像，UART0 在刷机前就打开抓一次性启动日志，同时看 CDC 节点和 shell 应答；
刷错可用 Ctrl-D 回 BOOTSEL。新工具 `scripts/rp2350/pump_repro.py`。

## 环境差异（必须写在结论前面）

- 本次板子是 **Pico 2 / RP2350 RV32 SMP（8 MiB）**；11-19 那两次用的是
  **ATK-DNRP2350AM**。两块板的 flash/LCD 不同，USB 时序是否一致未验证。
- 11-19 的镜像是**未提交的工作树构建**，当时的 USB 锁序 bug（`695db5b`）、
  `cdc` 无读者丢弃等问题今天都已修；本次树是 `5b32f05` 之后。
- 11-19 记录「UART 0 字节」时，GP0 那根线还是坏的（当日 §84 已结案），
  这能解释 UART 那一半，但解释不了 CDC 节点消失。
- 本次只有**刷机后的首次启动 + 20 次 ROM 暖重启**，没有 20 次真正拔电冷启动。

## 实验 A：40 000 次紧泵（复现 11-19 的写法）

`usb_cdc_wait()` 临时换成 `for (i = 0; i < 40000 && !tud_mounted(); i++) usb_cdc_poll();`
（无 40 周期空转），循环内每 10 000 次打一个 `plat_boot_stage()`。

```text
[C] usbwait polls=40000/40000 ns=2716 us=108677 bound
TinyOS/RP2350  rv32  uart0=115200  usb-cdc=up
tos> uptime
uptime: 2.320 s (2320 ms)  tick=1000 Hz  cpus=2  cpu=1  pid=2
```

- CDC 节点出现，shell 应答，**没有挂死**。
- 40 000 次紧泵耗时 **108.7 ms**（2 716 ns/poll）；配合上一张卡片的预 tick
  4.8 µs/poll（含 40 周期空转），预算因素彻底排除。
- `reboot_soak.py -n 20`：**20/20 PASS**，prompt 0.28–0.29 s，uptime 0.08 s。

日志：`build/pump-repro-tight40k.log`、`build/pump-repro-tight40k-reboot.csv`。

## 实验 B：完全去掉等待（复现 11-19 的 trap）

`usb_cdc_wait()` 直接 `return`。带符号 ELF。

```text
[U] usb hw=1 cfg=0 stage=0
TinyOS/RP2350  rv32  uart0=115200  usb-cdc=up
TinyOS shell (pid 1).
tos> uptime
uptime: 2.423 s (2423 ms)  tick=1000 Hz  cpus=2  cpu=1  pid=2
```

- **没有 trap**：UART0 上没有 `mcause=0x3`，CDC 正常，shell 正常；
  因此没有 `mepc` 可用符号反查。
- `reboot_soak.py -n 20`：**20/20 PASS**，prompt 0.18–0.19 s（比保留等待时快 0.11 s，
  正好是那 40 000 次紧泵的耗时）。

日志：`build/pump-repro-nowait.log`、`build/pump-repro-nowait-reboot.csv`。

## 已排除 / 未排除

| 假设 | 判定 |
|---|---|
| 40 000 次预算太大 | **排除**：实测 108.7 ms |
| 40 周期空转是必要条件 | **排除**：去掉它反而更快，板子正常启动 |
| 去等待必然触发 `TU_ASSERT` | **本次不成立**：41 次启动一次都没 trap |
| 11-19 的 UART 0 字节 | 当时 GP0 线坏，已结案；不能解释 CDC 消失 |
| 锁序反转（`695db5b`）| 今天刚修，可能是 11-19 挂死的混杂因素，无法回头验证 |
| 板子差异 / 未提交镜像 / 主机枚举时序 | **未排除**，这是本次不复现最可能的原因 |
| 真·冷启动才触发 | **未排除**：本次只有刷机首启 + 20 次暖重启 |

## 判定与后续

按 #29 的边界「不在没有现场的情况下删掉等待」，**保留 `usb_cdc_wait()` 原样**，
实验代码已全部撤回，板子已刷回正式镜像（`/proc/usbpoll` = 4 804 ns/poll，
72 388 次 mounted，正常）。issue 不关，风险继续记在 `docs/OPEN-ITEMS.md` 第 4 条。

要真正删掉等待，需要补的是**两块板各 20 次拔电冷启动**（人工），而不是再刷一次镜像。
若届时仍无 trap，才可以按 #29 的关闭条件删等待并重测。

回归：新增工具不影响内核；正式镜像的 `go build`、`build all` 与 `/proc/usbpoll`
已验证。
