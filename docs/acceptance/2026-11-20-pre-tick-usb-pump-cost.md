# 2026-11-20 预 tick USB 泵单价（issue #29）

> **再更正（约 17:45）**：ATK-DNRP2350AM 接台后第一次无等待启动就复现了 `mcause=0x3`（`usbd_control_xfer_cb` 的 `TU_ASSERT`），等待已恢复、#29 已重开。本卡描述的是当时的结论，已被 `docs/acceptance/2026-11-20-rv32-atk-trap-reproduced.md` 推翻。

> 后续（同日 17:28）：用户决定按 Pico 2 证据删除等待并关闭 #29。本文测到的
> `bootpump:`/`[C] usbwait` 仪表随等待一起撤掉，数据作为历史证据保留。


## 判据

问题不是“要不要保留 RV32 的枚举等待”，而是那个等待的**迭代上界是按什么单价算出来的**。
`usb_cdc_wait()` 的界是 `clk_hz * 3 / 260`（150 MHz → 1 730 780 次），注释声称
“约 1.5 s”；唯一的数量依据来自 `/proc/usbpoll` 的 2.66 µs/poll，而那是**运行中
shell 的稳态**（SMP 双核、中断开、1 ms tick 在排空 tx ring），不是等待所处的
**单核、中断关、tick 未起**的预 tick 环境。两个 regime 的单价不能互推。

关闭判据：在等待内部直接计时，分别取「有主机，提前 mounted」和「无主机，跑满界」
两组，判定 1.5 s 的说法是否成立。

## 环境

- ATK-DNRP2350AM / RP2350 Hazard3 RV32 SMP（板上 8 MiB 版本，
  `cat /proc/cpuinfo` 对应 `rpi-pico2-rv32-smp` 固件）。
- `COM27` = 板载 CDC（2e8a:000a，DTR 必须为 True）；`COM19` = UART0/CH343。
- 构建：`go run ./cmd/tos build -target rpi-pico2-rv32-smp`；
  刷机：`scripts/rp2350/bootsel_poke.py COM27 20 --dtr` 得到 BOOTSEL 盘，再用
  `scripts/rp2350/flash.py` 的 `flash()` 按 512 B UF2 块写入。

## 仪表

- `plat_usb_boot_pump(uint32_t out[5])`（`kernel/platform.h` 声明，`kernel/main.c`
  弱默认返回 0）：`out[0]`=ns/poll，`[1]`=实际 poll 数，`[2]`=界，`[3]`=是否
  mounted，`[4]`=总 µs。
- `usb_cdc_wait()` 自计时，`/proc/usbpoll` 第二行 `bootpump:`；启动行
  `[C] usbwait polls=.../... ns=... us=... mounted|bound`。
- `plat_boot_us()`（弱默认 0）读 TIMER0 `TIMERAWL`（1 MHz，32 位低字，
  ~71 min 回绕）。**第一次实现用了 `plat_now_ns()`，读到常数**：RV32 mtime 由
  `plat_timer_init()` 在 `plat_early_init()` 之后才 `SIO_MTIME_CTRL=0`、清零、重开，
  等待发生在那之前。第一版输出 `bootpump: 0 ns/poll, 72588 of 1730780 polls in 0 ms`，
  是一次干净的负面测量——循环在跑，时钟没跑。

## 数据

### 有主机（正常枚举，两次）

```text
tos> cat /proc/usbpoll
usbpoll: 2679 ns/poll (93318 polls per 250 ms)
bootpump: 4804 ns/poll, 72183 of 1730780 polls in 346822 us (mounted)

tos> cat /proc/usbpoll        # reboot 后
usbpoll: 2686 ns/poll (93075 polls per 250 ms)
bootpump: 4805 ns/poll, 71869 of 1730780 polls in 345340 us (mounted)
```

### 无主机（跑满界）

仅本次实验在 `usb_cdc_console_init()` 里、等待前后临时加
`usb_cdc_replug(0)` / `usb_cdc_replug(1)`；测完已撤回，正式镜像没有这两行。

```text
tos> cat /proc/usbpoll
usbpoll: 2706 ns/poll (92387 polls per 250 ms)
bootpump: 4792 ns/poll, 1730780 of 1730780 polls in 8295302 us (bound reached)
```

## 判定

| 项 | 结果 |
|---|---|
| 预 tick 单次 poll | **4792–4805 ns**（两次有主机 + 一次跑满界一致） |
| 稳态单次 poll | 2679–2706 ns（2.66 µs，与旧数据一致） |
| 预 tick / 稳态 | **1.8×**，确认两个 regime 不能互推 |
| 有主机时的等待 | ~345–347 ms，~72 000 次后 mounted |
| 跑满界的等待 | **8 295 302 µs = 8.30 s** |
| 注释的“约 1.5 s” | **错，实际长 5.5×**（3/260 隐含 0.87 µs/次） |
| 40 000 次 ARM pump | 按 4.8 µs 是 **192 ms**，不是“预算太大” |

**结论**：#29 里“有界 pump 在 Hazard3 上挂死是因为次数太多”的解释被否掉两次
（稳态换算 106 ms，预 tick 换算 192 ms）。同时发现现行等待本身是个 8.3 s 的上界，
这一点此前从未量过；本次把注释和 `docs/OPEN-ITEMS.md` 第 4 条更正。

**未闭的部分**：2026-11-19 那次“换成 40 000 次紧循环后 CDC/UART 全哑”的原因仍未定位。
本次没有重做那个实验，也没有删等待。`TU_ASSERT(0 == xferred_bytes)` 的断言定位仍然
有效，只是 pump 不是它的解释。

## 回归

- `go build ./...`、`go vet ./...`、`go test ./...` 全绿。
- `go run ./cmd/tos build all`：18/18 目标通过（新增的两个弱钩子不影响其他端口）。
- 板子最后刷回正式镜像（无 `replug` 实验代码），`cat /proc/usbpoll` 正常，
  `uptime` 正常归零。
