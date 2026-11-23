# ARM USB CDC 长输出：PendSV 延迟泵（#46，2026-11-23）

```text
TinyOS commit:     ea2d89d（ARM PendSV 延迟泵 + TX FIFO 512）
target/config:     esp32c3-generic；rpi-pico2 / rpi-pico2-smp / atk-dnr2350am /
                   rpi-pico（构建）；rv32 / rv32smp / arm / x86 QEMU
board/port:        ESP32-C3（COM4，回归）；RP2350 真机 NOT RUN（无板）
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4；QEMU 8.x
host:              Windows 11 / EKSOFT
T0:                PASS — go test -count=1 ./...
T1:                PASS — go run ./cmd/tos build all -> 18/18
T2:                PASS — smoke rv32/rv32smp/arm/x86；soak 3/3
T3:                C3 回归 PASS（accept.py 39/39）；RP2350 ARM / SMP NOT RUN
T4:                NOT RUN
```

## 问题

RP2350 用 USB CDC 做控制台时，长输出（`ls`、`crash`）在 **~192 B** 处截断，后面
的字节全部丢失。链路：

1. CDC TX 端点只有 `tud_task()` 会重新武装；
2. `tud_task()` 只在 `usb_cdc_poll()` 里，而 `usb_cdc_poll()` 的唯一周期调用点是
   `sched_idle` → `g_idle_poll`；
3. 只要有任务一直 runnable，`sched_idle` 永不进入 → 端点一直 busy →
   `tud_cdc_write_available()` 恒为 0 → `usb_cdc_putc()` 逐字节丢弃。

192 = 128 B FIFO + 已经交给端点的 64 B 包。系统不崩，只是控制台输出没了。

## 修法

tick 只置位，工作在“回到 Thread mode 前的那一个边界”执行：

| 位置 | 内容 |
|---|---|
| `arch/arm/start.S` | PendSV 向量原来跳 fault handler；改成和其他 handler 一样 `arm_trap_enter` + `tail_resume`，帧原样返回 |
| `arch/arm/platform.c` | `arm_handle_pendsv()` 调 `board_poll_deferred()`；`arm_handle_tick()` 只 `PENDSVSET` |
| `kernel/process.c` | `board_poll_deferred()` / `board_poll_installed()`（arch 不再直接碰 `g_idle_poll`） |
| `drivers/usb/rp2350/tusb_config.h` | TX FIFO 128 → 512 B |

三个条件都是有原因的：

- **只在用户帧上置位**：tick 打断内核时，PendSV 会在“半个系统调用、锁还握着”的
  中间跑；
- **只在 pending-exception 检查之后置位**：否则自己刚置的 PendSV 会被当成 pending
  exception，把这一拍该做的切换吞掉；
- **只在 CPU0**：控制台/ USB 按板级契约归 core 0。

PendSV 优先级 0xE0（最低），所以它**不能**抢占调度器或任何系统调用：没有内核锁、
中断是开的、USB（优先级 0）还能抢占它。这就是本端口能做到的“线程上下文”——内核
自己永远在 Handler mode，idle loop 在有任务时不进。idle 泵保留为兜底。

TX FIFO 512 是补充而不是修法：泵跑不进“产生字节的那个系统调用”里，所以单次写大于
FIFO 时无论泵多勤都会被截。树里最大的单次控制台写是一个 512 B 的 printf 缓冲或一个
256 B 的 cat 读块，512 正好覆盖。

RV32 端口（`TOS_RV_USB_IRQ=1`）本来就在自己的 IRQ 里跑 task，RP2040 在共享 shim 里
跑，都没动。

## 验证

| 项 | 命令 | 结果 |
|---|---|---|
| T0 | `go test -count=1 ./...` | PASS |
| T1 | `go run ./cmd/tos build all` | 18/18 |
| T2 smoke | `tostest smoke -target rv32\|rv32smp\|arm\|x86` | 4/4 PASS |
| T2 soak | `python scripts/soak.py --cycles 3 --targets rv32,rv32smp,arm` | 3/3 PASS |
| T3 C3 | `flash.py --build -p COM4` + `accept.py -p COM4 --no-reset` | 39/39 |

`rpi-pico2`、`rpi-pico2-smp`、`atk-dnr2350am`、`rpi-pico`（RP2040，M0+，验证新加的
PendSV 桩在 ARMv6-M 上也能汇编）单独构建通过。

## PendSV 本体的证据（QEMU arm，临时探针，未提交）

QEMU `arm`（mps2）没有 USB，也没有 `g_idle_poll`，正常跑不到这条路径。用一个临时
`g_idle_poll` 代替（**未提交**，`git checkout` 还原）：

```c
static void dp_probe(void) {          /* 数 IPSR==14 的调用 */
    unsigned ipsr;
    __asm__ volatile("mrs %0, ipsr" : "=r"(ipsr));
    dp_total++;
    if (ipsr == 14u) dp_pendsv++;
    ...
}
```

跑两个并发 spinner，让 tick 真的在用户态抢占/切任务：

```text
$ tostest smoke -target arm -commands "spin 400 &,spin 400,uptime"
spin[2] 1/6 at 905 ms
spin[3] 1/6 at 1164 ms      <- 两个进程被 tick 交替抢占
spin[2] 2/6 at 1306 ms
spin[3] 2/6 at 1564 ms
...
[dp total=320000 pendsv=2534]
SMOKE TEST OK
```

2534 次 ≈ 5 s 里每 tick 一次；无 HardFault、无 panic，之后 `uptime` 正常应答。这
覆盖了“tick 切任务 → `arm_resume` 改写 PSP → PendSV 尾链”的那条最难路径。

## 待板上确认（T3 NOT RUN）

无 RP2350 在机（Pico 2 / ATK 都不在）。Pico 2 上要跑：

1. `python scripts/rp2350/pico2_heartbeat.py -p <CDC 口> --dtr` → `HEARTBEAT OK 6/6`；
2. `ls /bin`、`ls -l /` → 输出完整，不再在 192 B 处停；
3. `cat /readme.txt`、`help` → 完整；
4. `run bench &` 期间敲键 → 有回显（就是“任务一直 runnable”的场景）；
5. 单核 `rpi-pico2` 与 `rpi-pico2-smp` 各一次。

失败时怎么读：`ls` 仍截断 → `tud_task` 还是没跑（查 IPSR/`g_usb_irq_count`、
PendSV 是否真被置位）；输出重复/丢行 → FIFO 512 与 `usb_cdc_putc` 的 flush 边界；
串口卡死 → PendSV 里 tud_task 与 USB IRQ 的嵌套（`g_in_task` + `usb_cdc_lock`）。

## 仍不做

- 不加 ARM kernel thread（只为 1 ms 泵不划算，见 `docs/OPEN-ITEMS.md` 关于
  `TOS_KTHREADS` 的条目）；
- 不在 USB IRQ 里跑 `tud_task`（历史上第一次中断就死锁）；
- 不在系统调用内部跑泵（PendSV 优先级低于 SVCall），所以单次 > 512 B 的控制台写
  仍会被截——这是有界丢字节，不是停摆。
