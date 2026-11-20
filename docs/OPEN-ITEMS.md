# 待办（只列**现在**还开着的）

这里只放还没做完的事。已关闭的条目——包括**结论和关掉它的那次实测**——都在
[`HISTORY.md`](HISTORY.md) 里，编号沿用原样（代码注释里写的"OPEN-ITEMS §23.3"之类的
引用，去 HISTORY 里找）。

> 原 §1（RP2040 4 KiB 窗口 + 软件栈守卫）已于 2026-11-17 验收关闭，结论和原始
> 回显在 `HISTORY.md` §43。

## 1b. Pico 2 上的 TinyOS 启动失败（已关闭 2026-11-18）

同一块 RP2350 上换成 **Pico 2** 后，GitHub v0.3.0 发布版和本轮 `atk-dnr2350am-smp` 都在
启动早期 HardFault（`PC=0xeffffffe`），USB CDC 不枚举；官方 MicroPython Pico 2 UF2
则能正常枚举。ATK-DNRP2350AM 上同一轮镜像通过 USB、1000 轮 `dd/rm` 和屏幕命令，
所以这不是 USB 线材/供电问题，也不能用这次 review 的 SMP/锁补丁解释。

**结论（2026-11-18）**：根因是三处叠加，都已修（`cff442f`）：

1. `pll_sys_init()` 在 **flash 里**切 `clk_sys`，切换瞬间的取指要重新对齐 flash 时序；
   W25Q32 有余量、W25Q64 没有。现在切换在 `.data.ramfunc` 里完成。
2. 板级开关 `xip_setup = boot2-w25q080` 在 ROM 已经把这颗 flash 配成 EBh 四线之后
   又写一遍 SRAM 状态寄存器并重设 `M0_TIMING`，把 XIP 弄坏。实测 ROM 的时序在
   150 MHz 下**本来就能用**（切换前后 4 KiB flash 校验和一致），所以内核现在完全不碰 QMI。
   这条开关连同 `boot2_w25q080.S`、`TOS_RP2350_BOOT2`、`boardconf` 的 `xip_setup`
   键已全部删除（2026-11-19）：没有任何板子用它，留着只会让人以为 Pico 2 需要它。
3. 首行串口输出用固定 12 MHz 猜分频（而 `clk_peri` 此时可能已是 150 MHz），
   于是上面两处故障都表现为“零字节”，把排障方向带偏。现在先测 `clk_sys` 再算分频。

现状：`rpi-pico2` 能启动到 shell，心跳级 6/6（`scripts/rp2350/heartbeat.py -p COM19
--target rpi-pico2`），`flash.py --smoke` 7/7。细节见 `docs/PICO2-VS-ATK-BOOT.md`、
`docs/acceptance/2026-11-18-pico2-boot-rp2350-cff442f.md`、`TESTING.md` §5.4。

仍未做：Pico 2 的 SMP / RV32 镜像真机验证；GP25 上的 1 Hz 心跳不亮（`led_set()` 仍
硬编码 GP3，见 issue #27）；`accept33.py` 的 24 项表只覆盖 ATK 四个镜像。

## 2. PIO 的"眼睛两项"（已关闭 2026-11-17）

汇编和回环都是自动判定、已通过；RP2040 和 RP2350 的板载 LED 都已看过：

- `blink` / `blink3`：RP2350 GP3、RP2040 GP25 均约 2 Hz 闪烁（RP2040 见 HISTORY
  §43，2026-11-17）；
- `ws2812`：GP4 接灯带，`pio tx 0 0 0xff0000` 后首像素亮红。Pico 上没有这个
  硬件，手头也没有灯带；视觉项 **NOT RUN**，字表/装入已由 `pio_accept.py`
  判定通过，按用户决定不再作为阻塞项。

注意 RP2350 那次"LED 没闪"是**引脚假设**问题（pad 在 2 Hz 翻转，见 HISTORY
§32.2），不是 PIO 的 bug；换板子先确认用户 LED 到底在哪个 GPIO。

另记一个验收脚本陷阱：PIO 的 32 字指令存储由同一 block 的四个 SM 共享。
RP2040 只有两个 block，早期脚本把 `blink`（PIO1 SM1）和 `ws2812`（PIO1 SM3）
都装在 origin 0，后者覆盖了前者前 4 字；`pio start` 会停在旧 PC，看起来像
PIO 驱动坏了。脚本已把 RP2040 的 ws2812 改到 PIO0 SM3，详见 HISTORY §43。

## 2b. A-0（SMP exec 并发）真机冒烟（RP2040 / RP2350 RV32 / RP2350 ARM 已验）

`kernel/elf.c` 的窗口边界和 `kernel/syscall.c::do_exec` 的 `path` 缓冲已改为 per-call
（HISTORY §44），QEMU `rv32smp` 上通过。

- **rpi-pico-smp**（2026-11-17，`7453bd6`）：10 轮双后台 `piperace 1 8 &`，
  **20/20 `piperace: ok`**；10 轮 `dd`+`rm` bad=0；无 `exec failed`/panic。
- **atk-dnr2350am-rv32-smp**（2026-11-17，`7453bd6`，ATK-DNRP2350AM Hazard3）：10 轮并发
  exec **20/20 ok**；10 轮 `dd`+`rm` bad=0；heartbeat UART/USB 6/6；
  `smpbench` 2.00x。
- **atk-dnr2350am-smp**（2026-11-17，`7453bd6`，ATK-DNRP2350AM Cortex-M33）：UART0 上
  10 轮并发 exec **20/20 ok**、10 轮 `dd`+`rm` bad=0、heartbeat 6/6；
  `smpbench` 2.06x。USB CDC 长输出（`ls`/`crash`）会截断，本轮以 UART0 为准。

## 5b. USB：还有三件事

0. **RV32 端口不能删 `usb_cdc_wait()`**（2026-11-19 已回退）。Hazard3 端口没有 USB
   中断，`usb_cdc_poll()` 就是控制器唯一的事件泵；删掉等待后 ATK Hazard3 首次启动
   在 `tos>` 之后立刻 trap（`mcause=0x3` breakpoint、`mepc=0x10013d0a`，该地址处是
   `li a0,0; ebreak`，即 panic 路径）。恢复后启动很快、CDC 立刻枚举。ARM 两个端口
   没有这个问题。**待查**：那个 `ebreak` 具体是哪个断言（当前发布 ELF 已 strip，
   需要一次带符号的构建）。

0b. **RP2040 reset → `tos>` 的 10 s 已解决**（`998efe5`）：共享 shim 用 RP2350 的
   bit 28 命名 USBCTRL，RP2040 是 bit 24，`unreset_block_wait()` 白跑 100 万次
   （实测 6.0 s）。改成按 `TOS_ARM_M0P` 选位后实测 **1.75 s**，且 CDC 瞬间枚举。
   注意这块板的 `TIMERAWL` 约 23 kHz，临时计时点不要当 µs 读。

1. **`usb_delay_ms()` 标定偏慢**：注释按每次迭代约 3 周期算，实测约 9 周期，
   所以“200 ms”实际约 614 ms（RP2040，`998efe5` 之后 `usb init` 里剩的那 0.7 s
   基本就是它）。要么按实测重算，要么只在 `SIE_STATUS.CONNECTED` 为真时才做
   D+ 掉电重连（冷启动不需要），见本条第 3 点。
2. **USB 序列号**：RP2350 两个端口已改成芯片唯一 ID（`d5e2b7d` 之后，
   ROM 的 `SYS_INFO_CHIP_INFO`，实测 ATK 为 `DB8EA31104F9F060`，M33 与 Hazard3 一致）。
   **RP2040 仍是 `"0001"`**：它没有 SYS_INFO，唯一 ID 要从 flash 发 0x4B 读，
   需要在 USB init 时走 SSI/ROM flash 序列，尚未实现；两块 RP2040 同时插仍会撞。
3. **主机侧枚举延迟**：固件侧 `usb init done` 已降到 158 ms（RP2040 实测），但
   Windows 把 CDC 配置起来仍要几秒——`usb_cdc_init()` 里 200 ms 的 D+ 掉电重连是
   给"刷完 UF2 后 ROM 的 MSC 节点"准备的，冷启动时并不需要。目标是只在需要时做，
   省掉每次冷启动的 205 ms（`usb_delay_ms(200)` + 5）。

   2026-11-20 读代码后的现状（**还没改**，因为判据要冷/暖对比，冷启动要人工拔电）：
   - `usb_clock_init()` 是**清** `RESETS` 的 bit（RP2040 bit24 / RP2350 bit28），
     不是打脉冲；真正把控制器状态清掉的是 `tud_init()` → `dcd_init()`。
   - 所以"进 BOOTSEL 没进过"这件事，唯一可能读到的证据是**进 `usb_clock_init()`
     之前**的 `RESETS.RESET` 这一位（USBCTRL 是否已被 ROM 放开）以及放开之后、
     `tud_init()` 之前的 `SIE_STATUS` 快照。两者都还是假设，得靠 A/B 数据说话。
   - 测法已落地：新增 `/proc/usbboot`（`g_usb_boot[8]`，三个 CDC 驱动在
     `usb_cdc_init()` 里填），输出进 `usb_clock_init()` 之前的 `RESETS.RESET`、
     `tud_init()` 之前的 `SIE_STATUS`/`SIE_CTRL`、焊盘写完之后与 D+ 重连之后的
     `SIE_STATUS`、以及掉电期间的总线状态。
   - **2026-11-20 已有两组数据（Pico 2 RV32 SMP）**：
     ① **BOOTSEL 刷机后首启**：`resets_at_entry=087db93f usbctrl_released=1`、
        `sie pre-tud_init=00000005`（VBUS + 全速）、`sie while down=00080001`；
     ② **ROM `reboot`（暖重启）**：**逐字节相同**。
     也就是说"USBCTRL 是否已被 ROM 放开"这一位**分不开**这两种情况——ROM 的普通
     启动路径同样把 USB 留在活的状态（它要靠 USB 判定进不进 BOOTSEL）。
   - 因此 A3 的前提被削弱：真正需要 200 ms 掉电重连的恰恰是这两种情况，而候选判据
     对它们没有区分度。**结论倾向"不做"**：这 200 ms 换不来正确性，只是每次启动
     省 200 ms；而判据若错，代价是 CDC 不枚举（本机 UART0 TX 又是坏的）。
     还剩③冷启动一组数据没取（要人工拔电，见第 5 条与 A4）；若冷启动也读
     `usbctrl_released=1`，这条优化直接关掉、不再讨论。
   - 风险：改错的后果是 CDC 不枚举，而本机 UART0 的 TX 现在又是坏的（第 5 条），
     等于板子要人工按 BOOTSEL 才能救。**动手前先修好 UART0**，或者确认
     `bootsel_poke` 那条 CDC Ctrl-D 通路在"枚举成功但 shell 不答"时仍然有效
     （2026-11-20 实测有效）。

4. **Hazard3 不能照搬 ARM 的有界 pump**（2026-11-19 实测）：把 RV32 的
   `usb_cdc_wait()` 换成 ARM 的 `USB_BOOT_PUMP`（40 000 次 `usb_cdc_poll()`）后，
   刷入即**启动挂死**——CDC 不枚举、UART 也是 0 字节，只能按 BOOTSEL 刷回。RV32 的
   `usb_cdc_poll()` 比 ARM 重得多（自己搬 tx ring + `usb_tx_drain()` + `plat_spin_*`），
   ARM 上“~250 ms”的数字不适用于它。

   **2026-11-20 已量**：`/proc/usbpoll`（新口，端口钩子 `plat_usb_poll_ns()`，
   其他端口弱符号返回 0）用 mtime 在固定时间片里测稳态单次 poll：
   `2666 / 2666 / 2659 ns`，即 **2660 ns/poll，250 ms ≈ 94 000 次**。
   注意这条数据**否掉了原来的解释**：40 000 次 × 2.66 µs = 106 ms，远达不到挂死的
   量级，所以那次挂死不是“预算太大”，更可能是 RV32 现有循环里那个 **40 周期间隔**
   被去掉导致的（ARM 的 pump 是紧循环）。

   **2026-11-20 那个 trap 解释清楚了**：`mcause=0x3` 是 **breakpoint = `ebreak`**，
   与访存错误无关。全镜像一共 47 条 `ebreak`，**全部**在 TinyUSB 的
   `TU_ASSERT/TU_VERIFY`（`tusb_verify.h:87` 的 `__asm("ebreak\n")`）里，
   所在函数清一色是 USB 栈：`tud_task_ext`(9)、`process_set_config`(5)、
   `tu_edpt_validate`(4)、`usbd_open_edpt_pair`(4)、`dcd_event_handler`(3)…，
   内核自己一条都没有。所以那次挂死**是 TinyUSB 的某个断言在本机被触发**，
   panic 打印的 PC 落在 USB 栈区间（当前镜像同区间是 `dcd_rp2040_irq`/`dcd_event_handler`）。
   触发器的具体断言没法定：11-19 那个镜像不在库里（本地实验，未提交），地址在现行
   镜像里落在指令中间。要钉死它，按下面的安全方式重做，**不要**在只靠 CDC 的时候刷
   （断言型 hang 在枚举前，UART0 又可能是哑的，只能人工按 BOOTSEL）：
   ① 用 `TOS_KEEP_SYMS=1` 构建带符号镜像；② 只改 `usb_cdc_wait()` 的循环写法，
   `ebreak` 的地址减去所在符号基址（`nm -n`）就能对到具体断言；③ 板子先接好
   能用的 UART0（见第 5 条）再刷。
5. **UART0 只丢发送（TX）方向——不是固件问题，是 GP0 这根线拉不高**（2026-11-20 结案）。
   现象：BOOTSEL 刷机后的那一轮起 UART0 连续 **0/20** 无输出，CDC **20/20** 正常，
   暖重启也救不回来（2026-11-19 RV32 两次，11-20 ARM 镜像稳定复现）。
   2026-11-20 用三个新自检口把它查透了（卡片：
   `docs/acceptance/2026-11-20-uart0-tx-diagnosis.md`）：
   - **收方向一直正常**：在 UART0 上发 `uptime`，回显从 CDC 出来——时钟分频、RX、
     `plat_init` 的 UART 初始化全都是好的，**原“ROM 改了 pad/时钟”的假设验伪**；
   - `/proc/uart0`：`CR/IBRD/FBRD/LCR_H`、引脚 mux（FUNCSEL=2）、焊盘
     （`iso=0 od=0 ie=1`）与正常启动逐项相同；
   - `/proc/uart0tx`：GP0 **空闲读回低**（UART 空闲本应为高），发送时只有 44/65536
     次采样越过阈值；
   - `/proc/gp0drive`：改由 SIO 直接驱动拉高（回读 `pin_mux=5`、`sio_oe` bit0 已置），
     **仍 2048/2048 读低**；同一时刻 gp1（适配器 TX）、gp25（LED）稳定读高，
     证明回读链路与整个 bank 正常。
   - `/proc/gp0drive` 之后做了**拔线对照**（人工 1 分钟，`build/gp0_watch.log`）：
     拔掉接在 GP0 的适配器线束 → `driving 1 high=2048 low=0`；插回 → 立刻
     `high=0 low=2048`。**一拔就高、一插就低**，同一 pad、同一固件。
   结论：**板侧与固件都没问题，是线束/适配器把 GP0 拉在低电平**（跳线错位、接到 GND、
   或适配器 RX 端有强下拉/分压）。修复动作在台面上：换线/换适配器、接对引脚
   （适配器 RX ← GP0，适配器 TX → GP1，共地）；修好后的判据是 UART0 能双向跑通，
   而不是只看 `/proc/uart0tx`。
   影响：闸门/启动日志不能把 UART0 当唯一见证通道，刷机后先看 CDC；
   但 UART0 的**输入**是可信的（本轮所有诊断命令都是经 UART0 发进去、CDC 读结果的）。
   附带结论：板子"卡死"时（CDC 枚举着但 shell 不应答）**Ctrl-D 仍然有效**——
   `scripts/rp2350/bootsel_poke.py <port> 25 --dtr` 能踢回 BOOTSEL，说明 tick 与 USB
   控制器还活着，只有 shell 不响应；1200-baud touch 对 TinyOS 无效（那是 pico-sdk 的
   复位接口）。

6. ~~**RV32 SMP：`reboot` 之后约一半的启动要 34 秒才出声**~~ —— **已修（2026-11-20）**。
   根因不是启动慢，是 **`reboot` 自己没能把芯片带下去**：两核以相反顺序抢同一对锁。

   - `usb_cdc_flush()`（RV32 端口）先取 USB 锁（SIO 自旋锁 30）再调 `usb_tx_drain()`，
     而 `usb_tx_drain()` 的既有顺序是 **先 `g_tx_lock` 再 USB 锁**。核 0 在 reboot 路径
     里持 USB 锁等 `g_tx_lock`，核 1 的 1 ms tick 在 drain 里持 `g_tx_lock` 等 USB 锁，
     互等死锁。命中率约一半，正好对应“tick 那一刻在不在 drain 里”。
   - 判据来自两处：`.noinit` 环形日志显示 `reboot` 进入后 431 ms 走到
     `usb_critical_enter()` 返回、**没有** `usb_tx_drain()` 的标记；`stall_probe.py`
     显示那 33 秒里 **CDC 设备节点一直在**（芯片没复位），而 33.56 s ≈ 2 × 16.78 s
     的“两个看门狗周期”只是巧合，不是“被清了两次”。
   - 修复：`usb_cdc_flush()` 改成先 `usb_tx_drain()`（自己按正确顺序加锁）、再单独取
     USB 锁做 FIFO flush；另外 `plat_reboot()` 置 `g_rebooting`，`plat_console_flush()`
     在该标志下不走 USB（要复位的机器不该等主机或等一把即将被复位的核握着的锁）。
   - 验证：46 次连续 `reboot` 全部 0.50–0.58 s（修前同口径约一半 34 s）；
     `gate_boot.py -n 10 --cpus 2` **10/10 PASS**（uart0 10/10、cdc 10/10、
     uptime 0.31 s）。卡片：`docs/acceptance/2026-11-20-rv32-reboot-stall.md`。
   - 顺带留下：`.noinit` 环形启动日志（只在上一轮没走到 shell 时打印）、
     `plat_boot_mark()`（`kmain()` 各阶段之间打点，端口可选的弱钩子）、
     `scripts/rp2350/stall_probe.py`。
   - 仍然成立的两条经验：**单槽标记会被下一次启动覆盖**（`WATCHDOG_SCRATCH(3)` 的
     `prev_phase` 往往是恢复启动留下的）；**ROM 自己在用 `WATCHDOG_SCRATCH(4..7)`**，
     自记只能用 0..3 或干脆用 `.noinit`。

## 6. goc32：工具链已自包含，剩下前端缺口

`goc32` 不再调用 clang/ld.lld：汇编器在 `goc32/internal/as`，链接器在
`goc32/internal/ld`，两者都已用 QEMU 上的 RV32 / Cortex-M3 程序验证过，并且
**生成的每一条非跳转指令都和 clang 汇编器逐字节比对过**（RV32 3,378 条 / Thumb
3,273 条）。native32 的 ARM 后自增问题（`*s++ = c` 丢旧指针，`gmsh` 的
`echo hi` 崩）已经修好，`gmsh` 在 Cortex-M3 上能跑 `echo hi` / `ls` / `free` /
`ps`。

**目标码大小不是 goc32 的目标**：发布镜像和预编译的 `msh` 仍由 clang 构建；
goc32 的定位是不装 clang 时的单文件开发循环（`goc32` + `tosput/rx`）。所以
RV32 上 goc32 版 `msh` 需要 20 KiB 窗口这件事，不需要去追平 clang 的 16 KiB。

功能宏已经跟着构建走：`tos build` 在 `build/build-config-<target>.txt` 里记下
用户程序的 `-D` 列表，`goc32` 自动导入（`TOS_THREADS`、`TOS_MAILBOX`、
`TOS_<FEATURE>`），并在启用时一起链接 `thread.c`/`thread_switch.S` 或
`kthread.c`。用 `-config` 打开 threads/mailbox/net 构建后，`goc32` 编出的
`gthread`（threadtest）和 `gmail`（mailtest）在 QEMU 上跑通；`-no-build-defines`
可以关掉自动导入。


还剩前端缺口：浮点 IR 和多数 GCC 扩展 asm，在代码生成之前就被拒绝。132 次
`user/*.c` 构建里有 111 次能走完整条流水线；其余是浮点/soft-float、扩展 asm，
以及没有 `main` 的库编译单元（`lib.c`、`syscall.c`、`thread.c` 等），不是
汇编/链接失败。`for` 初始化里的声明和带 clobber 的空 asm 屏障已经支持。

## 7. 明确"先不做"的设计项（有理由，别当遗忘）

- **PMP region 位图分配**（`ffs(~mask)`）：Hazard3 的 8..10/11..15 是硬连接线，
  今天实现就是死代码，等第二个从 bootloader 手里接管 PMP 的平台（HISTORY §34）；
- **内核工作队列 / 优先级继承**：现在只有一个延迟用户（USB 轮询）和很短的控制台/
  fs 临界区，出现"整文件拷贝期间持锁"这类真实需求再说；
- **MTD / PSRAM**：`plat_block_*` 够用，接第二种存储时再引入；PSRAM 到板后
  先按 §9 的"数据池"形态做，不要直接做成任务窗口；
- **`connected/disconnected`（`-ENOTCONN`）**：按原话不成立——控制台是 UART+CDC
  复合体，UART 永远"连着"；状态已经能从 `/proc/cdcfix` 看到。

## 8. 文档与博客

- `README.md` / `README.zh-CN.md` 是入口，长手册在 `docs/GUIDE.zh-CN.md`；
- 计划中的系列文章（设计取舍、每一次真机事故的复盘）放在 `docs/blog/`，目前还没开始写。

## 9. P4 的 PSRAM 最终做成什么样（2026-11-16）

PSRAM 本身还在 §7 的"先不做"里，但**形态和升级门槛先记下来**，免得板子到了以后
顺手把它做成一等任务窗口，再回头拆。

**目标形态：数据池，不是任务窗口。** `mem_domain` 加第二项时不带可执行标志，
`win_alloc()` 只接受可执行域——"PSRAM 上的地址不能 `exec`"是代码约束，不是文档
提醒。三个阶段，逐级解锁：

- **A 数据池（先做）**：文件系统 page cache、大块 buffer、网络/图像/音频接收缓冲。
  用户拿到的是句柄或偏移，不是可直接跳进去的地址；分配走独立的
  `kalloc_domain()`，不经过窗口分配器。
- **B 大块映射（中期）**：允许进程在自己的地址空间里挂一段 PSRAM（读写、不可
  执行），这样大 buffer 不用拷进 4 KB 窗口，窗口仍然留在片上 SRAM。
- **C 可执行窗口（最后，可能永远不做）**：任务窗口直接落在 PSRAM。要动 loader、
  `process_user_ok`、保护后端和调度，且每条路径多一个"快/慢窗口"分支。

**升级到 C 的门槛（三条全满足才讨论）**：

1. 有程序真的需要 >768 KB 的**可执行**镜像（现在最大的用户程序差两个数量级）；
2. 片上窗口区在高负载下真的耗尽——是 `ps` 能看到的碎片，不是猜的；
3. PSRAM 上的 `priotest` worst-case 仍可接受，且外部内存 MMU 页粒度 ≤16 KB。

**到板后必须先用 TRM 确认**：

- 外部内存 MMU 的页大小、能否同一物理页做 cached + uncached 双映射；
- cache line 大小、两个 HP 核的 cache 是否硬件一致（大概率不是）；
- PSRAM 的 DMA 可达性与对齐要求（不对齐就要 bounce buffer）。

两个已知的坑：ESP32-S3 的外部内存是 **64 KB 页**，若 P4 相同，PSRAM 上"最小窗口"
就是 64 KB，4 KB 窗口语义直接失效；cache miss 的延迟比片上 SRAM 高一个量级，
所以实时任务的栈和热路径不能放进去。计划里的 C3/C4 接缝
（`plat_dcache_clean/invalidate`、`mem_domain.flags`）就是为这三件事留的位置，
相关契约见 [`ARCHITECTURE-NEXT.md`](ARCHITECTURE-NEXT.md) 的 C3/C4 与 B-7。

## 11. C3 的 PWM/SPI/I2C 还差电气验收（2026-11-18）

2026-11-18 在 COM4 上把三条用户态路径跑通了。软件层结果：

- `pwm`：6 通道；`pwm 3 1000 50` → `pwm: GP3 = 1000 Hz, 50%`；`pwm 3 off` → `pwm: GP3 off`；
- `i2c`：1 条总线；`i2c scan 0`（100 kHz 与 400 kHz）→ `i2c: 0 device(s)`，无器件时不 panic；
- `spi`：1 条总线；`spi loop 0 -1 1000000` → `recv 00 00 00 00`（没有 MOSI→MISO 跳线，
  按预期 mismatch）；`spi loopback 0` → `internal loopback failed`（C3 没有内部回环位，
  文档已写明）。

还没做的是电气/外部器件层：

1. **LEDC**：用示波器或 LED 看 GP3 的 1 kHz/50% 波形——软件只能证明配置被接受。
2. **SPI2**：把 MOSI 跳到 MISO，`spi loop 0 -1 1000000` 应回读 `55 aa 00 ff`。
3. **I2C**：接一个从设备（BME280/SSD1306 等），`i2c scan 0` 应列出地址。

关闭条件：三项各一条实机记录（命令 + 输出或波形），写进 `docs/acceptance/`。
跟踪：issue #26（已按三步清单写好，谁在板边 5 分钟能关）。
