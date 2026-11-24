# USB 控制台写方向：环满时由写者自己等，而不是丢（2026-11-25）

```text
TinyOS commit:     本提交（撤回 `ba657e4` 的等待方案，记录结论）
target/config:     atk-dnr2350am / -smp、rpi-pico2 / -smp、rpi-pico / -smp、
                   arm（Cortex-M3，QEMU）构建；18 目标；arm QEMU smoke + soak
board/port:        Pico 2 / RP2040 / ATK 上 NOT RUN（结论来自 `694945a` 的
                   板测；短计数由另一侧实施并测试）
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

## 试过的两个方案

### 方案 A（已撤回）：环满时由写者等

`putc` 在环满、调用者是 Thread/SVCall 且中断开着时，自己循环调用
`usb_cdc_poll()` 推泵，直到环里有位置。**已撤回**，两个理由：

1. `kernel/plat/console.h` 的契约写得很明白：*"putc() may drop … blocking on a
   peer would stall every other core's output with it"*，而且"每个等待都必须有界"。
   在 `putc` 里等主机是违约的——`console_write()` 还持着 `out_lock`，SMP 上另一核的
   打印会跟着等。
2. 它把"谁推进泵"这个问题塞回了写路径，而这正是 `#46` 花了两轮才拆掉的东西
   （`tud_task` 不能进中断、不能无条件进系统调用）。

### 方案 B（采用）：`write` 如实返回短计数

- `putc` 环满时**返回失败**，不做任何等待；
- `console_write()` 在第一个失败的字符处停下，返回**实际接受的用户字节数**；
- `process_write()` 的 `FD_CONSOLE` 分支返回这个数，不再无脑 `done(r, len)`；
- 用户态 `write_all()` 重试；`cat` 的循环因此会在 tick 之间被抢占，泵每 1 ms
  把环排空，最终写完。

这个方案把"丢字节"从**静默**变成**可见且可重试**，符合 console 契约，也没有锁嵌套
风险。落地时需要三处一起改，否则 `cat` 会在第一次环满就退出：

| 位置 | 现状 | 要改成 |
|---|---|---|
| `user/lib.c` `write_all()` | `if (n <= 0) return n < 0 ? n : done;` —— **0 也当成终止** | 只有 `n < 0` 才失败；`n == 0` 时 `yield()` 后重试 |
| `kernel/process.c` `process_write()` `FD_CONSOLE` | `console_write(...); return done(r, len);` | `return done(r, (uint32_t)console_write(...));` |
| `kernel/console.c` `console_write()` | `void` | 返回接受数，第一个失败就停 |

计数必须**精确**：由 `con_putc()`/`plat_console_putc()` 返回是否接受（板级 6 个实现 +
`kernel/plat/console.h`，共享头要带 goc32 样例编译），不要用 `plat_console_tx_room()`
预判——room 在 SMP 上会与另一个生产者竞争，而且"CDC 没有枚举"必须区分于"环满"
（前者在纯 UART 板上意味着"无限"，不是 0）。

已知边界：`kprintf`、tick echo、fault/panic 没有重试语义，仍按契约丢弃——`(b)` 的判据
因此是"**短计数 + 重试后完整**"，不是"内核假装写完整"。

## 代价与风险

- 不额外占用内核栈、不在写路径里跑状态机；一次 `write()` 只返回它真正交出去的字节。
- 不走 `write_all` 的程序（直接调 `write`）必须自己处理 0，否则仍然会截断——这是短写
  语义的正常要求，需要在 `write(2)` 的说明里写清楚。
- 换行展开：`out_char_as('\n')` 先发 `'\r'` 再发 `'\n'`。环在两者之间满时，用户字节
  不算接受、但 `'\r'` 已经出去，重试会多一个 CR。无害，记录在案。

## 验证（本提交只有撤回）

- T0 PASS、T1 18/18、T2 `arm` smoke PASS、soak rv32+arm 2/2。
- 6 个 RP2 目标 + `arm`(M3) 构建 PASS。
- **T3 NOT RUN**：短计数由另一侧实现并测试；板上判据见
  `docs/OPEN-ITEMS.md` §5c —— `cat` 21 200 B 完整、`/proc/usbboot` 自身不丢尾行、
  `dropped = 0`（重试生效时 `dropped` 应很小甚至为 0），以及 RV32 两个镜像。
