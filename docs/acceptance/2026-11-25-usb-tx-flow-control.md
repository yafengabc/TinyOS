# USB 控制台写方向：环满时由写者自己等，而不是丢（2026-11-25）

```text
TinyOS commit:     本提交（`694945a` 的板测后续修）
target/config:     atk-dnr2350am / -smp、rpi-pico2 / -smp、rpi-pico / -smp、
                   arm（Cortex-M3，QEMU）构建；18 目标；arm QEMU smoke + soak
board/port:        Pico 2 / RP2040 / ATK 上本次改动 NOT RUN（结论来自上一条
                   694945a 的板测，本提交待复测）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
T0:                PASS
T1:                PASS — 18/18
T2:                PASS — arm smoke；soak rv32+arm 2/2
T3:                NOT RUN
T4:                NOT RUN
```

## 板测结论（`d7a3c8a`，Pico 2 四镜像，CDC 控制台 COM27）

完整的实测记录在
[`2026-11-25-usb-tx-ring-pico2-d7a3c8a.md`](2026-11-25-usb-tx-ring-pico2-d7a3c8a.md)。

| 检查 | 结果 |
|---|---|
| CDC 上传 21 200 B | CRC 通过，稳态 24.5 KiB/s |
| `lock timeouts / ungated` | 0 / 0 ✓（有界锁那半边成立） |
| `cat` 21 200 B，`rpi-pico2` | 0/3 完整（每次 255–271 B） |
| 同上，`rpi-pico2-smp` | 3/6 完整（失败时 276 B） |
| 同上，`rpi-pico2-rv32` | 0/3 完整（1 216 B） |
| 同上，`rpi-pico2-rv32-smp` | 3/3 完整 |
| `cat /proc/usbboot`（约 330 B） | 两个 ARM 镜像都丢最后一行 |
| 计数器 | `D` 与失败的 `cat` 等量增长（3 次失败 ≈ 64 KB） |
| 同一命令走 UART0 | 四条都完整 |
| 改动前 `0a30b69`（无 TX 环） | 每次出 576–581 B —— **不是本轮引入** |

## 根因

`cat` 每次 `read` 256 B、`write_all` 一次系统调用；内核 `console_write()` 在
**一次 SVC 里**把这 256 个字符逐个送进 `plat_console_putc()`。而泵（`usb_cdc_poll`）
在 rp2350 上只从 PendSV/idle 跑，PendSV 是 `0xE0`、**低于 SVCall `0x40`**，所以在
一次长写的过程中一趟都跑不了。

于是“单次系统调用能缓冲多少”就是上限：

- 改之前：`putc` 直接写 TinyUSB 的 512 B TX FIFO；
- `694945a` 之后：`putc` 只写本核 256 B 环 —— **上限从 512 掉到 255**，
  `dropped +64 263 / handed +1 783` 正好对上：整段 21 KB 里只有泵在后来的
  2 次 tick 里搬走了 ~600 B，其余全在环满时丢。

这不是锁的问题（`lock timeouts 0`），是**没有流控**：UART 之所以完整，是因为
`plat_console_putc()` 在 `FR.TXFF` 上**有界等待**，把生产者压住了；USB 这边没有
对应的背压。

## 为什么 ARM 与 RV32 单核数字不同

`drivers/usb/rp2350`（ARM）与 `drivers/usb/rp2350rv`（RV32）是两套写路径：
RV32 有一套 1 KiB 环 + 自己的 `usb_tx_drain()`，由 CPU 0 的 `usb_cdc_poll()` 排空
（这个端口没有 USB 中断），环满时在生产者侧静默丢弃。所以 RV32 的截断点是
`1024 + 端点里那一点` ≈ 1 216 B，而 ARM 是 `255 + …`。**本提交只改 ARM 与 RP2040
两个驱动**；RV32 那套要按它自己的模型（泵只在 CPU 0、可能在另一颗 hart 上写）单独改，
本轮不动，§5c(b) 在 RV32 镜像上仍是 NOT MET。

## 改法

`usb_cdc_putc()` 在环满时按**调用者上下文**决定：

- **Thread 或 SVCall、且中断已开**（程序自己的输出）→ 等，而不是丢：循环里调用
  `usb_cdc_poll()`，泵把环搬进 TinyUSB、flush，两个 poll 之间中断是开的，USB 中断
  完成传输释放空间。这样 `cat` 被主机速率（~64 KB/s）压住，21 KB 约 0.3 s 走完。
- **tick / fault handler / panic / 任何 PRIMASK=1 的上下文** → 维持原样：丢，计数。
  这些路径的延迟比一行日志重要；而且 PRIMASK 同时是递归保护——泵里（TinyUSB 回调、
  故障打印）再进 `putc` 一定是关中断的，不会再次调用泵。
- 放弃等待后置 `g_usb_tx_stalled`，直到环排空才允许再等：主机被拔掉时，代价是
  每次突发付一次 ~10 ms，而不是每字节一次。

`/proc/usbboot` 的 `console tx` 行因此变成
`dropped=D  handed=H  waited=W  stalled=S`（`W` = 被压住的写次数，`S` = 放弃次数）。

## 代价与风险

- 一次 `write()` 在主机慢时会被拉长（最大约 400 次 poll ≈ 10 ms 后放弃），并且
  `console_write()` 是**持 `out_lock`** 调用的：SMP 上另一核的打印会跟着等，
  但 `spin_lock` 不关中断、tick 与 USB 中断照常跑，等待是有界的。echo 路径用的是
  trylock，不受影响。
- 这是“在系统调用内部跑泵”的一个**受控例外**（只在写者被阻塞时跑一趟）。§5b 第 5 条
  里“不在系统调用内跑泵”的理由是 PendSV 抢不过 SVCall，所以泵本来就进不来；这里不是
  把泵塞进所有系统调用，而是让**被卡的写者**自己推一次。
- 仍不做的：为 USB 泵加 kernel thread；把 `tud_task` 放进 USB 中断。

## 验证

- T0 PASS、T1 18/18、T2 `arm` smoke PASS、soak rv32+arm 2/2。
- 6 个 RP2 目标 + `arm`(M3) 构建 PASS。
- **T3 NOT RUN**：需要板上复测（清单同时写在 `docs/OPEN-ITEMS.md` §5c）：

  1. `cat` 21 200 B：`rpi-pico2` 与 `-smp` 各 3 次，字节数/行数与源文件一致，结尾
     提示符在；`/proc/usbboot` 的 `dropped` 应为 0（`waited` 明显大于 0 是正常的，
     说明背压在起作用）。
  2. `cat /proc/usbboot` 自身（约 330 B）不应再丢最后一行。
  3. 拔掉/暂停主机再 `cat`：应在 `stalled` 上记一次、随后丢字节但不挂死；插回后
     恢复正常（`stalled` 归零）。
  4. RP2040 的 `accept.py` 与 1 ms gap 的 tosput 复测（输入方向不受本次影响，但要
     确认长输出与 `#46` 老用例没被写路径改动带坏）。
