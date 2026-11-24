# RP2040 控制台也改成自有 RX 环（2026-11-24）

```text
TinyOS commit:     本提交
target/config:     rpi-pico / rpi-pico-smp（驱动 + 板级）；18 目标构建；arm QEMU + soak
board/port:        RP2040 NOT RUN（无板）；ATK 上由板主复测
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
T0:                PASS
T1:                PASS — 18/18
T2:                PASS — arm smoke；soak rv32+arm 2/2
T3:                NOT RUN
T4:                NOT RUN
```

## 为什么改

`8e92fa4` 修掉 ARM SMP 控制台死锁时只动了 rp2350。RP2040 的
`drivers/usb/rp2040/usb_cdc.c` 还是老形状，而且更糟：板级 `plat_console_getchar()`
在**每个字节**上都调用 `usb_cdc_poll()`（跑整趟 `tud_task`），然后
`usb_cdc_getchar()` 再拿一次跨核锁、进一次 TinyUSB。这条路径是从 tick（中断已关）和
`sched_idle`（关中断）进来的，和 rp2350 那次死锁是同一类；同时还解释了 tosput 的吞吐
问题（见下）。

## 改了什么

| 位置 | 内容 |
|---|---|
| `drivers/usb/rp2040/usb_cdc.c` | 自有 RX 环（256 B）：`usb_task_run()` 在锁内把已完成字节搬进环；`usb_cdc_getchar()` 只出队，不取锁、不碰 TinyUSB；填充“先看 `tud_cdc_available()` 再取”，环满不吞字节 |
| `board/rpi-pico/platform.c` | `plat_console_getchar()` 不再调用 `usb_cdc_poll()`——状态机只从 USB 中断和 tick 跑 |

上下文不变量：填充只在 core 0（USB 中断只开在 core 0 的 NVIC），弹出在 core 0 且关中断，
所以是单核单生产/单消费，不需要原子操作。

## 顺带解释 tosput 的“减半才通过”

`tools/tosput/sender.go` 原来记的现象：RP2040 上 256 B/1 ms 时 2/3 的 42 KiB 传输在
chunk 24–27 死掉，NAK r5 解出来少一个字节；256 B/2 ms 时 9/9 通过（25–27 KiB/s）。

分析（按各段容量）：

- 接收链：USB FIFO 128 B（TinyUSB，128 B 满了就 NAK，**不丢**）→ tty raw 环 512 B
  （511 可用，满了**丢最新字节**，这是唯一会丢的一级）→ 读者；
- 每 tick（1 kHz）`tty_poll()` 最多排 1024 B，但**旧路径每字节**要跑一次
  `usb_cdc_poll()`（整趟状态机）+ 一次跨核锁——排一个 tick 的量（256 B）就要 0.5–1.3 ms，
  已经超过一个 tick。于是 256 B/ms 的到达速率大于服务速率，511 B 环在几毫秒内被填满，
  中间某个字节被丢 → base64 错位 → 块“少一个字节” → NAK r5 → 重传。
- 2 ms 间隔把到达速率减半，服务能跟上，所以 9/9 通过。停等协议只保护“写 flash 期间”
  （那时主机在等 ACK），**不保护一行的传输窗口**，所以这个余量是必须的。

改完之后，关中断的排空退化成一串字节数组弹出，**每字节的状态机开销没有了**。因此
1 ms 间隔值得在 RP2040 上重测；默认仍留 2 ms（吞吐已经在协议上限 ~26 KiB/s，缩短间隔
不涨吞吐）。

## 验证

- T0 PASS；T1 18/18；T2 `arm` smoke PASS；soak rv32+arm 2/2 PASS。
- **T3 NOT RUN**：RP2040 上要复测两件事——(a) 常规 `scripts/rp2040/accept.py`；
  (b) 把 `writePaced` 的 gap 临时改回 1 ms，传 42 KiB 与 128 KiB 各 3 次，看
  `NAK` 是否消失；再用 `RAW_Q` 的丢字节计数（如果加）确认不是环满。
