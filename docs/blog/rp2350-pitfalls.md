# RP2350 踩坑笔记

> 状态：进行中。已写引言、两个核的对比、BOOTSEL、SWD/UART；后面继续补 USB CDC 和
> 内存保护那几条。文中每个“坑”都尽量带上仓库里的复现脚本或代码位置，方便对照。

## 1. 为什么是 RP2350

现在的嵌入式 SoC 多少都带了内存保护：ARM M 系列的 MPU、RISC-V 的 PMP。我借着这个思路
Vibe 了一个迷你“Linux”——整个内核一百多 K，大头是 littlefs 和 TinyUSB，调度器加系统
调用本身只有几十 K。最早在 ESP32-C3 / RP2040 上写，后来找 RP2040 板子的时候撞见了
RP2350：同一颗芯片、同一块板，烧哪种镜像就得到“双核 RV32”或者“双核 M33”（二选一，
启动时由镜像/OTP 决定），而且便宜到离谱——淘宝 15 块包邮，还带 16 MB flash。于是开发
重心就挪了过来。一路搞下来百感交集，把踩过的坑写下来，一吐为快。

## 2. 两个核：M33 与 Hazard3

Cortex-M33 是“产品级”的东西：Thumb-2、带 FPU、内存保护是 PMSAv8（8 个 region + MAIR），
异常优先级、调试寄存器都齐全，写起来心里有底。

Hazard3 就素得多：没有 FPU（RP2350 datasheet 原话是 "Hazard3, which lacks the F
extension"），PMP 只开了 NAPOT——Hazard3 本身是参数化的，TOR 的比较器在 RP2350 这颗的
实例化里没开，而且最小保护粒度是 32 字节（`PMP_GRAIN = 3`）。更坑的是取指：pre-v1.1 的
Hazard3 把“一条 32 位指令跨两个 PMP 区域”判成 fault，哪怕两边都给 X；Hazard3 **v1.1**
的 release notes 才把它放宽（"allow instructions to straddle two PMP regions which
both have the necessary permissions"）。叠上页粒度的 NAPOT 映射，RVC 压缩指令就会偶尔
踩到“页边界前 2 字节”，这个后面单独讲——它最后让我们把用户态的压缩指令整个关掉了
（[`docs/OPEN-ITEMS.md`](../OPEN-ITEMS.md) 里有完整推导）。

有点反直觉的是：RISC-V 这边遇到的 bug 反而比 ARM 少。我的解释是“能错的地方少”：M33 要
照顾 PMSAv8 的 region 编码、异常优先级和尾链、`CONTROL`/PSP、TrustZone、FPU 惰性压栈，
每一个都是独立的失败模式；Hazard3 只有 M/U 模式 + PMP + 一小撮 CSR。坑少，但更深——
没有像样的调试手段时，一个跨页取指 fault 只能靠地址尾巴是 `0xffe` 去猜。

## 3. 一开始要频繁 BOOTSEL

RP2350 的“烧写器”其实就是它自己的 mask ROM：上电或复位时按住 BOOTSEL，它才挂出一个
FAT 卷（新的 ROM 叫 `RP2350`，老的叫 `RPI-RP2`），把 UF2 拖进去就写 flash 并重启。
它没有 UART 下载协议，也没有“运行中拉一下某个引脚就进 ROM”的握手。

C3 那边完全是另一个世界：USB-Serial/JTAG 加上 esptool，自动进下载模式，一条命令刷完，
手不用碰板子。我一开始按 C3 的节奏开发，结果在 RP2350 上一天要“拔线 → 按住 BOOTSEL →
插上 → 拖 UF2 → 等枚举 → 重开串口”几十次。我们这块 ATK 板的 CH343 的 DTR/RTS 并没有
接到 BOOTSEL/RESET（四种组合都试过），所以“脚本拉 DTR 自动复位进 ROM”这条路根本不存在。

后来靠三件事把这件事变得能忍：

1. **常驻的刷机看守**：脚本盯着 UF2 盘符，一出现就拷贝，同时抓启动日志。关键是串口必须
   **在拷贝之前**打开——启动横幅是一次性流，晚一秒就永远看不到了。一次按键 = 一次完整的
   “刷机 + 抓日志”。见 [`scripts/rp2350/flash.py`](../../scripts/rp2350/flash.py)
   （RP2040 那边的对应物是
   [`scripts/rp2040/autoflash.py`](../../scripts/rp2040/autoflash.py)，它的注释里也记着
   “DTR/RTS 没有接到 BOOTSEL”这一条实测）。
2. **固件自己的逃生口**：控制台上发 Ctrl-D（`0x04`），固件调用 ROM 的
   reboot-to-BOOTSEL，UART 和 USB CDC 都认。更妙的是 1 ms tick 在前 30 秒里会自己偷看
   UART0 的 RX FIFO，所以哪怕 shell 还没起来、甚至内核半死，Ctrl-D 也能把它踢回 ROM——
   不用按键。实现见
   [`board/rp2350-generic/platform.c`](../../board/rp2350-generic/platform.c) 的
   `BOOTSEL_MAGIC_CHAR` / `rom_enter_bootsel()`；自动化脚本是
   [`scripts/rp2350/bootsel_poke.py`](../../scripts/rp2350/bootsel_poke.py)（`--dtr` 时
   连复位键都不用按）。
3. **记住“刚刷完那一次”的状态**：`/proc/usbboot` 记录 ROM 在进我们的 USB 初始化之前留下
   的 `RESETS`/`SIE` 快照（[`kernel/procfs.c`](../../kernel/procfs.c)）；USB 初始化里那
   200 ms 的 D+ 掉电重连，就是给“主机上还挂着 ROM 的 MSC 节点”这种情况准备的。

教训：RP2040/2350 这条链上没有“软件复位进 ROM”的硬件路径，全自动刷机要么靠人按一次，
要么靠镜像自己还活着。C3 天生没有这个问题——不是 RP2350 更差，是它的 ROM 只做存储盘，
不做串口下载器。

## 4. SWD：买了 DAPLink、PicoProbe，最后靠 UART

按说明书，SWD 应该比 printf 高级得多：halt、看寄存器、下断点、改内存。我买了 DAPLink，
又买了 PicoProbe（拿另一块 Pico 当调试器），最后发现真正解决问题的还是 UART。原因有五条：

1. **板上不一定有现成的调试口**。我们手上这块 ATK 板没把 SWD 引成排针（要用得自己飞线），
   Pico 2 倒是有标准 3-pin debug，但要额外接线、还要确认没和别的功能复用。
2. **双核**。RP2350 是两个 hart，attach 时要选调试哪个；一个 halt 住，另一个还在跑，
   继续把你要看的状态改掉。
3. **一 halt 就不是那个系统了**。我们要抓的是 1 ms tick、USB 的 1 ms 帧、SMP 竞争——
   停下来看的那一刻，时序已经不存在了。
4. **attach 本身的时序**。镜像进了 HardFault、或者关中断死循环时，经常得“按住 reset 再
   连”，比拔插还慢；而这恰恰是最需要调试器的时候。
5. **信息维度不对**。SWD 给你的是寄存器快照，给不了“调度器认为没人可跑”“tty 的队列没被
   排空”“USB 端点没被重新武装”。我们的 bug 大多是这一层：系统还活着，但某个状态机停了
   ——这种东西在 register view 里是一片 0 和 1，在 `/proc` 里是一行字。

所以 UART 被我们做成了“第一现场”：

- **两条独立控制台**（UART0 + USB CDC）。一个哑了另一个还在：有一轮 ATK 的 UART0 TX
  完全没有输出，最后是靠 CDC 才定位到是线束而不是固件——“单通道静默”本身不是诊断
  （[`scripts/rp2350/stall_probe.py`](../../scripts/rp2350/stall_probe.py) 就是把这三种
  静默拆开的工具）。
- **启动打点**：每个阶段 `[A]..[E]`、`[p] ...`（`plat_boot_stage()`），加上每核 tick
  计数，卡在哪一步一眼可见。
- **`.noinit` 计数器**：tick 数、idle 停在哪一步、USB 停在哪一步、哪把锁在自旋、谁在等
  ——复位不清零，下一轮启动先把上一轮的死前现场打出来。板子“死了”也能留下遗言
  （[`kernel/cpu.c`](../../kernel/cpu.c) 的那组 `TOS_NOINIT` 变量）。
- **`/proc` 诊断**：`/proc/cpuinfo`、`/proc/dev`、`/proc/usbboot`、`/proc/stack`，
  以及 `ps` 里的栈高水位。
- **几个小脚本把“静默”拆开**：`stall_probe.py` 同时盯 UART、CDC 节点是否存在、CDC 是否
  应答；[`scripts/rp2350/coldboot_watch.py`](../../scripts/rp2350/coldboot_watch.py) 看
  冷启动的顺序；[`scripts/rp2350/pump_repro.py`](../../scripts/rp2350/pump_repro.py) 在
  刷机后同时验证两条控制台。

SWD 不是没用——真要看某条指令执行时的寄存器，它还是最快的。但它救不了“系统级”的 bug。
RP2350 上真正省时间的，是**多留一条独立的控制台，让固件自己说话**。

## 5. 一把锁、两条核、一个“关中断的读”

ATK-DNR2350AM SMP 上，跑十六轮 `prot heap` 再叠一个 `deep 3`，板子停住：UART 一个字节
都没有，最后是看门狗把它拉回来的。现场只有一份“死得很安静”的镜像。

根因是三条同时成立：

1. `sched_idle` 是先 `plat_irq_disable()` 再调 `tty_poll()` 的——tty 的队列不是可重入的，
   所以那段必须关中断（[`kernel/process.c`](../../kernel/process.c)）；
2. `tty_poll()` 一路走到 `plat_console_getchar()` → `usb_cdc_getchar()`，而它当时要**取
   USB 锁并进入 TinyUSB**（[`drivers/usb/rp2350/usb_cdc.c`](../../drivers/usb/rp2350/usb_cdc.c)）；
3. 另一核正握着那把锁跑一整趟状态机。

于是关中断的核对着一把跨核的锁自旋，tick 再也不来，整个系统只剩看门狗。

修了三处：

- **驱动自有的 RX 环**。`tud_task()` 本来就在锁内、在 core 0 上跑，让它把已完成的字节搬进
  一个 256 字节的环；`usb_cdc_getchar()` 只从环里出队，不取锁、不碰 TinyUSB。这其实不是
  “绕开死锁”，而是把 `kernel/plat/console.h` 里 console 的 A 类契约真正恢复了——那一条
  写的是“`getchar()` 每次最多一次寄存器读，因为它跑在 `tty_poll()` 里、中断已关”。
- **SIO 门首次超时可恢复**：门只保护三条指令的窗口，超时说明持有者已经不在了；打印
  `[GATE] cpu N lost gate, forcing release` 后释放并重试，第二次超时才 panic。
- **STALL 的 `greg=` 改读 `SPINLOCK_ST`**：读 `SPINLOCK[31]` 这个动作本身就会把门**抢走**，
  诊断不能干扰它要诊断的系统（顺便把这个寄存器名挪进了 `arm_memmap.h`）。

附带一条诊断改进：fault handler 进来时记一个 `site 6`，这样“死在 handler 里”和“卡在
调度器里”在 stall 记录里能分开。

复盘时还发现一个小问题：RX 环满时，原来的写法是先从 TinyUSB 取出一个字节、再判断环满，
那个字节就丢了。改成先看 `tud_cdc_available()` 再取——控制台输入里丢一个字节，可能正好
丢掉一个换行。

RP2040 的驱动（[`drivers/usb/rp2040/usb_cdc.c`](../../drivers/usb/rp2040/usb_cdc.c)）
当时还是“取锁 + 进 TinyUSB”的老写法，而且更糟：`plat_console_getchar()` **每个字节**都
调一次 `usb_cdc_poll()`（整趟状态机）、`putc()` 每个字节取一次锁、`usb_cdc_lock()` 还是
**无界自旋**。后来按同一套改掉了（自有 RX 环、TX 环、有界获取），这是同一课的第二遍。

### 写方向：把“打印一行”从锁里拿出来

读方向修完，写方向仍然在锁里：`putc()` 和 `flush()` 都要取跨核 SIO 门，而它们从 tick、
echo 和 fault handler 进来（全都关中断）。`prot heap`×16 叠 `deep 3` 恰好是“大量 fault
+ 大量打印”，只要另一核正握着门跑状态机，这一核就在打印路径上排队。

现在每核一个 256 字节的 TX 环：`putc()` 只入环——不取锁、不进 TinyUSB、满了记数丢弃；
泵（锁内、core 0）把字节搬进 TinyUSB 并 flush。**为什么不是一个环加一把小锁**：单环要
跨核原子，而 M33 的 LDREX/STREX 只有本核语义（`OPEN-ITEMS §24.4` 已经因此出过事）；
每核一个环把每个生产者/消费者对变成单侧，发布顺序“字节 → `dmb` → head”就够了。泵按快照
取，所以“泵自己在 fault 里打印”只是把字节推到下一趟。

环上线后第一轮板测就给了一课：**`cat` 21 KB 只剩前 255 字节**（单核 3/3，SMP 1/3），
`/proc/usbboot` 自己都丢最后一行，而 `lock timeouts` 是 0——锁没问题，是**没有背压**。
`cat` 一次 `write()` 在同一个 SVC 里把 256 个字符全塞进去，而泵只从 PendSV/idle 跑，
PendSV（`0xE0`）抢不过 SVCall（`0x40`），一整次长写里泵一趟都进不来。于是“单次系统调用
能缓冲多少”就是全部上限：改之前是 TinyUSB 的 512 B FIFO，改成环之后是 255 B——
比 UART 还退了一步。UART 之所以完整，是因为它在 `FR.TXFF` 上**有界等待**，把生产者
压住了；USB 这条路径缺的正是这一条。

第一版补法是“环满时让写者自己等：循环调 `usb_cdc_poll()` 推泵”，但这条路被否决了：
`kernel/plat/console.h` 的契约第一条就写着 *putc may drop … blocking on a peer would
stall every other core's output*，而 `console_write()` 还持着输出锁——在 `putc` 里等
主机是把 `#46` 刚拆掉的“谁推进泵”又塞回了写路径。

真正采用的补法是把失败**告诉上层**：`putc` 环满就返回失败，`console_write()` 在第一个
失败处停下并返回实际接受的字节数，`write` 的返回值因此是短计数，用户态 `write_all()`
重试——`cat` 的循环会被 tick 抢占，泵每 1 ms 排空，最终写完。代价是所有直接调 `write`
而不检查返回值的程序仍然会截断，但这是短写语义的正常要求，而且**丢字节从静默变成了
可见**。教训不是“环要更大”，而是**缓冲不解决生产者比消费者快的问题**：要么背压，
要么把没写下去的部分如实告诉调用者。

第二条是更危险的：抢门超时的旧代码执行 `usb_lk_reg = 0`，此后整个启动剩余时间的 USB
访问都变成无锁，只留一行 `[ULK!]`。这不是挂死，是**静默损坏**——两个核同时进 tu_fifo，
症状是偶发丢字节、看不出原因的 panic。现在超时只记数并返回 `USB_LK_FAILED`，调用方用
`USB_LK_OK()` 判断后跳过本次操作；`flush()` 改成 `trylock`，门忙就把字节留给下一次泵，
panic 路径也不再等另一核。两个计数器（`lock timeouts`、`console tx dropped`）露在
`/proc/usbboot` 里，下一次失败自己会说话。

这一节和“192 字节”那节其实是同一件事的两面：USB 驱动里凡是“**被别的上下文从锁外调用**”
的入口，都必须是只碰内存的；一旦它想拿锁、跑状态机，就会在 SMP 上把另一条时间线拖死。

## 证据索引

| 现象 | 仓库里的位置 |
|---|---|
| RP2350 的 ROM/UF2 流程，1200-baud touch 只对 pico-sdk 固件有效 | `scripts/rp2350/flash.py` 开头的说明 |
| 板上 DTR/RTS 没接 BOOTSEL（四种组合实测） | `scripts/rp2040/autoflash.py` 开头 |
| Ctrl-D → ROM BOOTSEL（UART 与 CDC 都认） | `board/rp2350-generic/platform.c` 的 `BOOTSEL_MAGIC_CHAR` |
| 刷机后 ROM 留下的寄存器快照 | `/proc/usbboot`（`kernel/procfs.c`） |
| 三种“静默”的区分 | `scripts/rp2350/stall_probe.py` |
| 死前现场（复位不清） | `kernel/cpu.c` 的 `TOS_NOINIT` 计数器 |
| 跨 PMP 条目取指的完整推导与上游 v1.1 的放宽 | `docs/OPEN-ITEMS.md`（RP2350 RV32 一节） |
| “关中断的读”与 USB 锁的 SMP 死锁（三处修 + 一个丢字节的补丁） | `docs/acceptance/2026-11-24-atk-arm-usb-console-deadlock-6b15b8b.md`、`drivers/usb/rp2350/usb_cdc.c` |
| 写方向的自有 TX 环、锁超时不再降级无锁（两处计数器在 `/proc/usbboot`） | `docs/acceptance/2026-11-25-usb-tx-ring-bounded-lock.md`、`drivers/usb/rp2350/usb_cdc.c` |

## 还没写的

- USB CDC 的“192 字节”：TX 端点只有 `tud_task()` 会重新武装，任务一直 runnable 时
  idle 泵就永远不跑（issue #46 的完整链路）。
- 页粒度的 PMP 映射与块预算：为什么 `rcc` 的 96 KiB 窗口在一半的基址上要降级。
- 32 字节最小 region：为什么“插个 4 字节桥接条目”这种想法不成立。
