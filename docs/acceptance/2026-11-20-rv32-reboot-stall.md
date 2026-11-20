# RV32 SMP：`reboot` 之后约一半的启动要 34 秒才出声（2026-11-20）

```text
commit:            （见本轮提交）
change:            usb_cdc_flush() 的加锁顺序与 usb_tx_drain() 相反，两核互等死锁；
                   修复后 46/46 次重启在 0.50–0.58 s 回来
baseline:          fix 前 5/8 慢（`gate_boot.py` 同一判据）；fix 后 10/10 PASS
target/config:     rpi-pico2-rv32-smp / defaults（对照：rpi-pico2-smp）
board/port/image:  Raspberry Pi Pico 2，CDC=COM27，UART0=COM19（CH343）
toolchain:         go 1.x, clang 23.1.4, python 3.13
host:              Windows 11 / EKSOFT
T1:                fix 前：RV32 SMP 10 次 `reboot`，4 次 34.01 s，其余 0.50–0.57 s
T2:                fix 前：ARM 镜像 10 次，10/10 在 0.38 s（同一台、同一根线）
T3:                停顿时 `usb_cdc_flush()` 已进入、`usb_critical_enter()` 已返回、
                   `usb_tx_drain()` 没有返回 → 卡在 `plat_spin_acquire(&g_tx_lock)`
T4:                `usb_cdc_flush()` 持 USB 锁再调 drain；drain 先取 g_tx_lock 再取
                   USB 锁 → 两核锁序相反（死锁，不是慢）
T5:                fix：先 `usb_tx_drain()`（自己加锁），再单独取 USB 锁做 FIFO flush
T6:                fix 后 16/16 + 20/20 次 `reboot` 全部 0.50–0.58 s；闸门 10/10 PASS
T7:                fix 后 `gate_boot.py`：uart0 10/10、cdc 10/10、cpus=2、uptime 0.31 s
logs:              scripts/rp2350/stall_probe.py（双通道观察）、gate_boot.py 的 CSV
notes:             根因是既有的锁序反转，与 UART/时钟/ROM 无关；ARM 端口无此问题，
                   因为它的 `usb_cdc_flush()` 从不调用 drain
```

## 现象

`reboot` 之后，约一半的轮次线上整整 33.6 s 没有任何字节，然后突然出现完整启动日志。
33.56 s = **2 × 16.78 s**（`WATCHDOG_RELOAD = 0x00ffffff` µs），所以当时以为是
“两次启动都卡住、各被看门狗清掉一次”。

```text
[ 1/8] uart=  34.01 s      [ 5/8] uart=   0.51 s
[ 2/8] uart=  34.00 s      [ 6/8] uart=  34.03 s
[ 3/8] uart=  34.00 s      [ 7/8] uart=   0.51 s
[ 4/8] uart=   0.50 s      [ 8/8] uart=   0.51 s
```

## 真正的形状：芯片根本没复位

`stall_probe.py` 同时盯三件事：UART0 的输出、Windows 里 CDC 设备节点是否在、
以及往那个节点发 `uptime` 有没有回应。慢轮次的结论是：

```text
      0.01  cdc         COM27          <- 节点一直在
     33.53  cdc-answer  <SerialException>
     34.14  cdc         gone
     34.49  UART0 出现 `tos>`
```

**CDC 节点在这 33 秒里一直没有消失**。芯片若真的复位了，D+ 上拉会掉、Windows 会在
毫秒级内看到断开。所以这段时间不是“启动慢”，而是 **`reboot` 自己没能把芯片带下去**。
UART0 上敲 9 次 `uptime` 也全无回应——因为 `console_shutdown()` 已经把控制台关了。

## 卡在哪一句

之前的 `WATCHDOG_SCRATCH(3)` 单槽标记有个致命缺陷：**卡住那次之后，恢复成功的那次
启动会把它覆盖掉**，所以 “prev_phase=17” 往往是恢复启动留下的，不是卡住那次的。
换成 `.noinit` 里的环形日志（`boot_step_t`，每条记 phase + `RP_UART_FR` +
`g_clk_hz` + IBRD/FBRD + `CLK_SYS_SELECTED` + mtime），追加而不是覆盖，才拿到真相：

```text
ph=0x60  fr=0000013f  clk=08f0d568  div=00000051/00000018  t=002e3336   <- 进入 plat_reboot()
ph=0x83  fr=00000197  ...                                              <- usb_cdc_flush 进入
ph=0x84  fr=00000197  ...                                              <- usb_critical_enter 返回
（没有 0x85）
```

0x60 → 0x84 只花了 **431 ms**，之后什么都没了。0x84 的下一句就是
`usb_tx_drain()`，而 `usb_tx_drain()` 第一件事是 `plat_spin_acquire(&g_tx_lock)`。

顺带否掉了两个中间结论：

- `[A]`/`[B]` 是**卡住那次自己打印的**（`clk=150 MHz`、`div=0x51/0x18` 全对），
  所以“UART 没时钟 / 波特率不对”不成立；
- 环形日志显示有一次启动**完整走到了 `process_start_shell`**，所以“启动卡在某处”
  也不成立——是 `reboot` 卡住了，不是启动。

## 根因：两个核以相反顺序抢同一对锁

```c
/* 修前：usb_cdc_flush() */
usbpm = usb_critical_enter();     /* 先拿 USB 锁（SIO 自旋锁 30） */
usb_tx_drain();                   /* 里面：g_tx_lock -> USB 锁 */
tud_cdc_write_flush();
usb_critical_exit(usbpm);
```

`usb_tx_drain()` 的既有顺序是 **先 `g_tx_lock` 再 USB 锁**（文件里那段注释也这么写）。
`usb_cdc_flush()` 把它倒过来用，于是：

- 核 0（正在 `reboot`）持有 USB 锁，等 `g_tx_lock`；
- 核 1 的 1 ms tick 在 `usb_tx_drain()` 里持有 `g_tx_lock`，等 USB 锁。

两个核互等，谁都不动。核 1 是在 `plat_irq_save()` 之后卡住的，于是 tick 停了、看门狗
没人喂，最后被清掉。命中率约一半，正好对应“tick 那一刻有没有在 drain 里”。

## 修复

```c
/* 修后 */
usb_tx_drain();                   /* 自己按 g_tx_lock -> USB 锁 的顺序加锁 */
usbpm = usb_critical_enter();
tud_cdc_write_flush();
usb_critical_exit(usbpm);
```

另外加了一道保险：`plat_reboot()` 先把 `g_rebooting` 置 1，`plat_console_flush()` 在
这个标志下**不走 USB**。要复位的机器不该等主机把最后几个字节取走，也不该等一把
即将被复位的核可能还握着的锁；UART 那条腿（串口抓包看到的那条）照常 flush。

ARM 的两个端口没有这个问题，因为它们的 `usb_cdc_flush()` 只做
`tud_cdc_write_flush()`，从不调用 drain。

## 验证

```text
16 次 reboot：16/16 在 1.05 s（fix 前同口径 ~50% 慢）
20 次 reboot：0/20 慢，min 0.50 s，max 0.58 s，mean 0.52 s
gate_boot.py -n 10 --cpus 2：10/10 PASS
  uart0 boot 10/10, cdc boot 10/10, cdc never opened 0/10, uptime 0.31 s
```

## 留下什么

- `.noinit` 环形启动日志（`boot_step_t` / `boot_log_dump()`），只在**上一轮没走到
  shell** 时打印，正常启动一个字节都不多说。
- `plat_boot_mark(n)`：`kernel/main.c` 的弱钩子，在 `kmain()` 各阶段之间打点，
  端口有环形日志就记、没有就什么都不做。
- `scripts/rp2350/stall_probe.py`：双通道观察重启（UART0 输出 + CDC 节点 + CDC 应答），
  下次再遇到“看不见的 30 秒”先用它。
- 教训：**单槽标记会被下一次启动覆盖**；“两个看门狗周期”这种整倍数很容易把人骗到
  “启动被清了两次”上，而真正的事实可能是“一次都没复位”。

## 试过但没结论的（保留，避免重走）

- `WATCHDOG_SCRATCH(7)` 想记“ROM 重启返回了没有”（写 `"RBOO"`/`"FAIL"`），
  读回 **`00000004`**：**ROM 自己在用 scratch 4..7**，所以自记只能用 0..3（现在
  完全改用 `.noinit`）。
- 加 7 个阶段标记后命中率从约 1/4 变成 8/8——当时以为是“仪表移动 bug”，实际是
  死锁本来就对时序敏感（标记改写了 reboot 前那一刻的指令时序）。
- 短路 `rp2350_flash_detect()`（JEDEC 探测的 XIP 关/开窗口）后仍 7/8 慢：与探测无关。
