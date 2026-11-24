# USB 控制台：TX 改自有环，锁超时不再降级成无锁（2026-11-25）

```text
TinyOS commit:     本提交
target/config:     atk-dnr2350am / -smp、rpi-pico2 / -smp、rpi-pico / -smp、
                   arm（Cortex-M3，QEMU）构建；18 目标；arm QEMU smoke + soak
board/port:        ATK / Pico 2 / RP2040 均 NOT RUN（本机无板）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
T0:                PASS
T1:                PASS — 18/18
T2:                PASS — arm smoke；soak rv32+arm 2/2
T3:                NOT RUN
T4:                NOT RUN
```

## 背景：两个只在 SMP 上显形的洞

`8e92fa4` 把“关中断的读”从 TinyUSB 里摘出来之后，控制台还剩两条同类路径：

1. **写方向还在锁里。** `usb_cdc_putc()` / `usb_cdc_flush()` 都要取跨核 SIO 门，
   而它们是从 tick、echo、fault handler 进来的（全都关中断）。另一核只要正握着门
   跑一整趟状态机，这一核就在打印路径上自旋；门是有的、界是有的，但“打印一行”
   变成一个要等另一条时间线的事情。`prot heap`×16 + `deep 3` 正是“大量 fault +
   大量打印”的组合。
2. **抢门超时会永久关掉互斥。** 旧代码在 `n >= USB_LK_SPINS` 时执行
   `usb_lk_reg = 0`，此后整个启动剩余时间的所有 USB 访问都走“无锁”分支，
   只留一条 `[ULK!]`（仅 `TOS_DIAG`）。这不是挂死，是**静默损坏**：两个核同时
   进 tu_fifo，症状是偶发丢字节 / 缓冲区错乱 / 看不出原因的 panic。

RP2040 的驱动是同一份形状，而且更糟：`usb_cdc_lock()` 是**无界自旋**，`putc()`
逐字节取它。

## 改了什么

| 位置 | 内容 |
|---|---|
| `drivers/usb/rp2350/usb_cdc.c`、`drivers/usb/rp2040/usb_cdc.c` | 每核一个 256 B 的 **TX 环**：`putc()` 只入环（不取锁、不进 TinyUSB、满了计数丢弃），泵在锁内把字节搬进 TinyUSB 并 flush |
| 同上 | 锁超时**不再** `usb_lk_reg = 0`：计数后返回 `USB_LK_FAILED`，调用方用 `USB_LK_OK()` 判断并**跳过本次操作**；RP2040 的 `usb_cdc_lock()` 也从无界自旋改成同一套有界获取 + 深度计数 |
| 同上 | `usb_cdc_flush()` 用 `usb_cdc_trylock()`：门空闲就同步 flush，被占就把字节留给下一次泵（≤1 ms）——panic 路径不再等另一核 |
| `usb_cdc_tx_room()` | 报本核环的剩余空间，不再无锁读 TinyUSB 的 FIFO 计数 |
| `drivers/usb/*/tusb_shim.c` | 中断 trampoline 拿不到门就跳过这一趟（SIE 的 INTS 仍然锁存，下一趟或 tick 兜底会服务同一个事件） |
| `kernel/procfs.c` | `/proc/usbboot` 新增 `lock timeouts / ungated / console tx dropped / handed` 四项 |

### 为什么每核一个环，而不是一个环加一把小锁

单环需要跨核原子（Cortex-M33 的 LDREX/STREX **只有本核语义**，这一点
`OPEN-ITEMS §24.4` 已经踩过）。每核一个环把每个生产者/消费者对变成单侧：
本核写本核的 head，泵是唯一读者。发布顺序是“先写字节、`dmb`、再写 head”，
泵读 head 后再 `dmb`，SMP 下不需要任何锁或原子指令。泵按快照取，因此
“泵自己触发 putc”（fault handler 或 TinyUSB 回调里打印）只是把字节推到下一趟。

### 代价

- `.bss`：SMP 镜像多 512 B，单核多 256 B。
- 输出延迟从“`putc` 里直接进 FIFO”变成“下一次泵”，tick/PendSV 下 ≤1 ms。
- `flush()` 在门被占时不再等：字节不丢，但要等到下一趟泵；如果板子随后 halt
  （panic），那一行可能没出去。这是“不在关中断的路径上等锁”的直接代价。

## 板测结果（2026-11-25，Pico 2 `d7a3c8a`）

- `lock timeouts / ungated` = **0 / 0**：有界锁这半边成立。
- `cat` 21 200 B：**`rpi-pico2` 3/3 只到 255–271 B**（`-smp` 1/3 失败），
  `/proc/usbboot` 自身也丢最后一行；3 次后 `dropped +64 263`、`handed +1 783`。
  同一命令走 UART 21 621 B 完整。
- 结论：256 B 环把“一次系统调用能缓冲的量”从 TinyUSB 的 512 B 压到 255 B，
  而泵在一次长写里跑不了（PendSV `0xE0` 抢不过 SVCall `0x40`）。
  修法与复测清单见
  [`2026-11-25-usb-tx-flow-control.md`](2026-11-25-usb-tx-flow-control.md)。

## 验证

- T0 PASS、T1 18/18、T2 `arm` smoke PASS、soak rv32+arm 2/2 PASS。
- 6 个 RP2 目标与 `arm`(M3) 单独构建 PASS（两套 `#ifdef` 分支都编到）。
- **T3 NOT RUN**：本机无板。待跑清单同时写在
  [docs/OPEN-ITEMS.md](../OPEN-ITEMS.md) 的 5c 节，交板主执行。板上要看的是：

  1. Pico 2 / ATK（CDC 控制台）跑完长输出与 `prot heap`×16 + `deep 3` 之后
     `cat /proc/usbboot`：`lock timeouts 0  ungated=0`，`console tx dropped`
     正常应为 0（除非主机确实没读），`handed` 随输出字节增长。
  2. #46 的回归：一次输出 >16 KiB（`cat` 大文件）不截断。
  3. RP2040：`scripts/rp2040/accept.py`，以及把 `tools/tosput/sender.go` 的
     gap 临时改回 1 ms 后 42 KiB / 128 KiB 各 3 次，看 `NAK` 与
     `console tx dropped` 是否都为 0。
