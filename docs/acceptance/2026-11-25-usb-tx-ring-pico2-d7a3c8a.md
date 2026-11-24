# USB 控制台 TX 环（`694945a`）在 Pico 2 上的板级验证：长输出仍会截断（2026-11-25）

```text
TinyOS commit:     d7a3c8a（被测镜像）；TX 环本身来自 694945a
target/config:     rpi-pico2 / -smp / -rv32 / -rv32-smp（Pico 2，RP2350）
board/port:        Pico 2 —— UART0 COM19、USB CDC COM27
toolchain:         go 1.27.1 windows/amd64；clang 23.1.4
T0:                NOT RUN
T1:                4/4 PASS
T2:                NOT RUN
T3:                FAIL —— 待跑清单 (b) 不满足；(a) 的 N/U 达标，D 不达标
T3 (方案 1):       PASS(ARM 两镜像 + RV32 两镜像) —— 见文末“方案 1”与
                   “方案 1 扩展到 RV32”两节
T4:                NOT RUN
```

## 验证方法

按 `docs/OPEN-ITEMS.md` §5c 的要求全部走 **CDC 控制台（COM27）**，UART0 只用来读
计数器（原因见下）。载荷是 21 200 B 的 ASCII 文本（400 行，`line 000` … `line 399`），
用 `tosput` 上传后 `cat` 整份，主机侧逐字节比对；同一条命令在 UART0 上作对照。

## 结果

| 镜像 | 控制台驱动 | `cat` 21 200 B | 实际收到 |
|---|---|---|---|
| `rpi-pico2` | `usb/rp2350`（新环） | 0/3 完整 | 255–271 B |
| `rpi-pico2-smp` | `usb/rp2350`（新环） | 3/6 完整 | 失败时 276 B |
| `rpi-pico2-rv32` | `usb/rp2350rv`（**未改**） | 0/3 完整 | 1 216 B |
| `rpi-pico2-rv32-smp` | `usb/rp2350rv`（**未改**） | 3/3 完整 | 21 621 B |

UART0 对照：四条镜像都是完整 21 621 B，首尾行与提示符齐全 —— shell 与 `cat` 本身
没有问题，丢字节只在 CDC 镜像路径上。

## 计数器（ARM 两镜像）

`cat /proc/usbboot` 在 CDC 上本身就会被截断（330 B 的输出，最后一行 `console tx …`
在 6 次里丢了 4 次），所以数值从 UART0 读：

- 空载：`lock timeouts 0  ungated=0`，`console tx dropped=0  handed=30`。
- 3 次 `cat` 之后：`dropped=64 263  handed=1 813`。3 份文件共 ~64.8 KB，说明失败
  的运行里几乎整份被丢。
- SMP 6 次 `cat`（3 次失败）之后：`dropped=64 226  handed=66 500`，
  64 226 ≈ 3 × 21.2 KB，与“3 次全丢、3 次全成”完全对得上。

即 (a) 的 `N = 0`、`U = 0` 达标，但“主机在读”的前提下 `D` 并不为 0。

## 对照：改动之前（`0a30b69`，无 TX 环）

同一 `cat` 每次送出 576–581 B，也不完整。所以“单次大输出丢尾巴”不是 `694945a`
引入的；这轮把边界从 ~576 B 变成 256 B（新环的大小），**(b) 的目标没有达成**。

## 观察到的机理

`putc()` 入环满了就计数丢弃，泵在 tick 里每拍只搬几十字节。`cat` 一次 `write()`
如果远大于 256 B，环外的那部分就整段丢掉，连行尾和提示符一起丢。SMP 上出现
“3/6 成功、3/6 全丢”的双峰，说明成败取决于另一核是否恰好在写的过程中把环排空；
单核则没有任何并发排水，稳定失败。

## RV32 不在这次改动的范围内

RV32 镜像用的是 `drivers/usb/rp2350rv/usb_cdc.c`，`694945a` 只改了 `usb/rp2350` 与
`usb/rp2040`。所以 RV32 的 `console tx dropped/handed` 恒为 0 是**计数器不存在**，
不是“没丢”；RV32 单核的 1 216 B 截断是它自己的旧路径行为，RV32 SMP 完整则说明
那条老路径在双核下能排完。

## 结论

§5c 的 (b) 在 Pico 2 上 **NOT MET**：(ARM 单核 0/3、ARM SMP 3/6、RV32 单核 0/3 完整)。
赔偿不了的原因是“单次写大于环容量时有界丢弃”，按 §5b 第 5 条的边界这属于已知限制，
但它恰好是 (b) 想验的东西，因此不能按 PASS 收口。

下一步有两条互斥的路，需先确认 `cat` 的写粒度再选：

1. 若 `cat` 确实是“一次大 `write()`”，修 `console_write`/`cat` 让它分块并在块间
   让出（副作用最小，UART 行为不变）；
2. 若要保持任意大写都完整，就得让 `putc()` 在环满时阻塞或重试 —— 这条路会把
   “不在关中断路径上等锁”的收益吐回去一部分，需要单独评估。

## 未覆盖

- ATK（COM14/COM26）不在场，`atk-dnr2350am` / `-smp` 的同一项未测。
- RP2040 两镜像（`drivers/usb/rp2040` 同样改了 TX 环）未测，§5c (c) 仍未跑。
- `run prot heap` + `deep 3` 的组合只在 ARM 单核上跑过一次，`N` 仍为 0。

## 方案 1：写路径反压 + 短写重试（2026-11-25，本提交）

上节第 1 条路。确认 `cat` 的写粒度是 256 B 缓冲 + `write_all()` 循环，所以问题不是
“单次大写”，而是“生产者快于泵”：`plat_console_putc()` 对 CDC 是满则丢，
而泵（`usb_txq_drain()`）只在 idle 与 PendSV 里跑；单核用户任务忙写时 tick 落在
Handler 模式、不 pend PendSV，泵被饿死，尾部整段丢。

改动（6 个文件）：

- `kernel/console.c` 新增弱默认 `plat_console_can_take()`（恒 1，其他平台行为不变）
  与 `console_write_some()`：环满时返回短计数；同一个“填充块”连续 250 ms 没有得到
  空间就转入 `stall_drop`，之后的字符照旧交给 `putc()` 丢弃（保证不无限等待）。
- `kernel/process.c` 的 FD_CONSOLE `write()` 改用 `console_write_some()`。
- `board/rp2350-generic/platform.c` 定义 `plat_console_can_take()`：CDC 已配置且
  本核环装不下 `need` 字节时返回 0。
- `user/lib.c` 的 `write_all()` 把 0 字节当作“对方慢”而不是“对方挂了”：`yield()`
  后重试。旧代码在 0 上直接返回已完成字节数，短写方案会因此截断（本轮踩过一次）。
- `board/rp2350rv-generic/platform.c` **故意不提供**该钩子：RV32 没有 PendSV
  等价边界，配速只会撞 250 ms 截止后丢弃（实测加上后 SMP 从 3/3 掉到 2/3）。

### 复测结果（四镜像，`build/tinyos-rpi-pico2*.uf2`）

| 镜像 | `cat` 21 200 B 经 CDC | 实际收到 |
|---|---|---|
| `rpi-pico2` | 9/9 完整 | 21 621 B，首尾行 + 提示符 |
| `rpi-pico2-smp` | 3/3 完整 | 21 621 B |
| `rpi-pico2-rv32` | 9/9 完整（见下节） | 21 621 B |
| `rpi-pico2-rv32-smp` | 9/9 完整（见下节） | 21 621 B |

- 四条镜像在 CDC 口关闭时 `pico2_heartbeat.py` 都是 **6/6**（UART0）。
- ARM 两镜像的 `console tx dropped` 在长输出前后不再增长（此前每次失败增长
  约 21 KB）；单核那次总量 8 488 是“CDC 口被打开但不读”窗口内的丢弃，
  随后 3 轮 `cat` 全完整且计数不再变化。
- 把 COM27 打开但一直不读（Windows 驱动缓冲写满）：UART0 上的 `cat` 仍输出完整
  21 621 B，提示符在 2.19 s 返回（≈115200 波特率的传输时间），没有出现
  “每块等 250 ms”的抖动，也没有卡死 —— 250 ms 截止只在真正无空间时触发。
- RV32 两镜像的 `console tx dropped/handed` 恒 0 仍是“没有这套计数器”，不代表没丢；
  RV32 的完整数据来自 CDC 侧实收字节数（见下节）。

## 方案 1 扩展到 RV32：tick 返回用户态作为 PendSV

ARM 的泵有两个入口，第二个（PendSV，每 tick 一次）才是“任务一直可运行时泵还活着”
的原因。RV32 只有 idle 这一个入口，而 `cat` 持续可运行时 idle 永远轮不到，所以
21 200 B 稳定停在环容量附近（1 216 B ≈ 1 KiB 环 + FIFO 在途）。

改动：

- `arch/rv32/platform.c` 的 tick 分支在 `process_resched()` 之后、返回帧之前，
  若该 tick 打断的是用户态、且是 0 号核、且板子装了轮询钩子，就**开中断**调用
  `board_poll_deferred()`，返回前再关中断（`mret` 用帧里的 MPIE 恢复用户态中断）。
- `board/rp2350rv-generic/platform.c` 补上 `plat_console_can_take()`：以
  `usb_cdc_tx_room()`（该端口报的是 TinyUSB FIFO 余量，比 1 KiB 环更紧）判满。

为什么开中断是安全的（注释里也写了）：嵌套 trap 来自 M 态，`process_resched()`
只允许“打断用户态的帧”消费调度请求，所以不会在泵中间切任务或迁移；USB 临界区
自己 `plat_irq_save()`，本核不会和自己的 tick 里的 `usb_cdc_tx_kick()` 死锁。
不开中断则相反：tick 会被压在泵后面，而 `plat_timer_tick()` 是按“醒来时刻 + 1 ms”
重装比较器，每次泵都会从时钟里偷时间。

### 复测结果

| 镜像 | 结果 | 备注 |
|---|---|---|
| `rpi-pico2-rv32` | 3/3 + 6/6 = **9/9 完整** | 每轮 21 621 B、首尾行 + 提示符 |
| `rpi-pico2-rv32-smp` | **9/9 完整** | 同上 |
| `atk-dnr2350am-rv32` / `-rv32-smp` | 构建通过 | 板不在场，未板测 |

- 两条镜像 CDC 口关闭时 heartbeat 均为 **6/6**；`time advances 1 s` 分别
  `1.116 s` / `1.119 s`，说明每 tick 多跑一次泵没有让时钟变慢。
- 变基到上游“短计数是正确背压”（`2c96f75`）之后重建，四镜像各复跑 3 轮：
  heartbeat 6/6，`cat` 全部 21 621 B 完整（`dropped` 不随轮次增长）。
- 把 COM27 打开但不读，UART0 上的 `cat` 仍完整 21 621 B、提示符 2.19 s 返回。

### 已知副作用与未覆盖

- CDC 口打开而主机不读时，写路径会在 250 ms 内表现为“慢”，之后恢复为丢字节；
  这是有意的兜底，不是无限阻塞。
- RV32 镜像上 `tosput` 走 COM27 会在握手阶段失败：`usb/rp2350rv` 的
  `cdc_has_reader()` 在 DTR 为低时不发数据，而 `tosput` 打开端口后把 DTR 拉低。
  `pyserial`（DTR 默认拉高）工作正常；上传改用 COM19 完成，与本轮改动无关。
- ATK 两镜像用同一 `board/rp2350-generic`，ATK 的 RV32 两镜像用
  `board/rp2350rv-generic`：四个都构建通过，但板不在场、未板测；RP2040 未测。
