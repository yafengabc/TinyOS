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
硬编码 GP3，见 issue #27）；`accept33.py` 的 24 项表原先只覆盖 ATK 四个镜像，2026-11-25 起也带 Pico 2 四镜像（littlefs 总量按各自 board.conf 取、镜像里没有的程序记 skipped）；两条镜像都还没用新参数在板上跑过，第一次结果要回填这一节。。

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
  `smpbench` 2.06x。USB CDC 长输出（`ls`/`crash`）会截断，本轮以 UART0 为准。（2026-11-23 已修，`ea2d89d`，待 Pico 2 复验，见本文件第 5b 节第 5 条）

## 2c. RP2040 板级验收待跑（2026-11-23）

RP2040（`rpi-pico` / `rpi-pico-smp`）从 2026-11-23 起重新是 T3 必跑项
（`docs/TESTING.md` §5.3、`AGENTS.md` 的硬件验收策略）。入口是一条命令：

```sh
python scripts/rp2040/accept.py --uf2 build/tinyos-rpi-pico.uf2
```

**2026-11-25 已补跑**（`5260d28`，Pico `E0C912952D54`，COM19 UART / COM29 CDC）：
`rpi-pico` **UART0 30/30 + CDC 30/30**（长输出经 USB 完整，#46 判据在 RP2040 单核成立）；
`rpi-pico-smp` **UART0 30/30**，CDC **29/30**（仅 `fault isolation` FAIL：crash 后 10 s 内
没读到结尾提示符；同板 UART0 上 3 轮 crash 均正常打印 HardFault、回提示符、uptime 连续
不复位，见 §5d(b)）。人工步骤只有按键进 BOOTSEL；长输出必须走板载 CDC，UART0 不算
USB 证据。证据与镜像 sha：`docs/acceptance/2026-11-25-rp2040-open-items-5260d28.md`。
原 2026-11-23 的 `NOT RUN` 见 `docs/acceptance/2026-11-23-rp2040-back-in-matrix-1cc53c4.md`。

**Ctrl-D 进 BOOTSEL（2026-11-25，板测 PASS）**：以前一直不通，根因是 ROM 的
`reset_usb_boot()` 走 `watchdog_reboot(wrapper, SRAM_END, 10)`，而本板故意停了
watchdog tick（RP2350 用 REBOOT2，所以那边一直好用）。现在 `clocks_init()` 启动
1 MHz tick，Ctrl-D 只保留一条 ROM 路径：`bootsel_poll_uart()` 里调用（本内核的 idle 在
SVCall 里，IPSR=0x0b，没有可达的 thread mode——远程版本的 `IPSR==0` 条件因此永远
不成立，实测 Ctrl-D 无反应），先停 SysTick，再 `reset_usb_boot(0,0)`；scratch 魔法、
SOF 强制触发和复位后二次调 ROM 都已删除。实测：UART0 发 `0x04` →
`[bootsel] ROM reset_usb_boot()` → 0.81 s 出现 `RPI-RP2`，从该卷刷回同一镜像成功。
操作与失败采集见 [`RP2040-BOOTSEL.md`](RP2040-BOOTSEL.md)，证据见
`docs/acceptance/2026-11-25-rp2040-ctrl-d-bootsel.md`。 panic 之后的 Ctrl-D 同样可用：请求在 `bootsel_poll_uart()` 里直接
交给 ROM，而 panic 自旋正是反复调用它（不再依赖已停的 tick）。

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

0c. **RP2040 有界 pump 四项真机数据已补齐，#31 于 2026-11-20 关闭**：
   `rpi-pico-smp` 人工 BOOTSEL 刷机，PC 口 16 次（8 次精确打点 0.52–0.58 s）、
   hub 5 次（0.36–0.47 s）、充电头无主机 5 次（0.35–0.45 s），0 trap；
   `flash.py --smoke` 7/7，GP25 目视 6/6。这一组数据是在加第 2 条的唯一序列号
   （早期 4Bh flash 命令）**之前**测的；序列号后来已实现并双板实测（见第 2 条）。
   卡片：`docs/acceptance/2026-11-20-rp2040-boot-pump-31.md`。

1. **`usb_delay_ms()` 标定偏慢**：注释按每次迭代约 3 周期算，实测约 9 周期，
   所以“200 ms”实际约 614 ms（RP2040，`998efe5` 之后 `usb init` 里剩的那 0.7 s
   基本就是它）。要么按实测重算，要么只在 `SIE_STATUS.CONNECTED` 为真时才做
   D+ 掉电重连（冷启动不需要），见本条第 3 点。
2. **USB 序列号（2026-11-20 三项都已完成）**：RP2350 两个端口从
   `SYS_INFO_CHIP_INFO` 取芯片 ID（ATK 实测 `DB8EA31104F9F060`，M33/Hazard3 一致）；
   RP2040 改为从 flash 发 4Bh 读 W25Q16JV 的 64 位 UID——`plat_flash_unique_id()`
   弱钩子 + RAM 里的 SSI 命令，板上 CDC 序列号从 `0001` 变成
   `E6609CB2D34AB733`，重启后不变。注意这块 flash 的 4Bh 应答**没有 08h 长度字节**，
   UID 直接在 `rx[5..12]`（与 pico-sdk 一致）。**双板不撞号已实测**：第二块
   RP2040（16 MiB flash、只有 CDC）同插得 `464A383332112312`，与第一块的
   `E6609CB2D34AB733` 不同，重启后均不变。读不到时回退 16 个 0。卡片：
   `docs/acceptance/2026-11-20-rp2040-unique-usb-serial.md`。
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

4. **Hazard3 不能照搬 ARM 的有界 pump —— 重开并保留等待（2026-11-20）**：
   **结论先读**：曾按 Pico 2 的 28 次冷启动删掉等待并关 #29；ATK-DNRP2350AM
   接台后第一次无等待启动就复现 `mcause=0x3 mepc=0x10016362`
   （`usbd_control_xfer_cb` 的 `TU_ASSERT(0 == xferred_bytes)`），
   等待已恢复、#29 已重开并以「跨板差异无法定位」结案。
   卡片：`docs/acceptance/2026-11-20-rv32-atk-trap-reproduced.md`。
   下面保留 11-19 至 11-20 的取证过程作为历史。
   原问题：把 RV32 的
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

   **2026-11-20 补上预 tick 那一档（上面那条只是稳态，不是同一 regime）**：新增
   `plat_boot_us()`（读 TIMER0 `TIMERAWL`，1 MHz；**不能用 `plat_now_ns()`**——RV32
   mtime 由 `plat_timer_init()` 在 `plat_early_init()` 之后才清零重开，boot console
   阶段读到常数，第一版探针因此报出「72 588 次 / 0 ms」），`usb_cdc_wait()` 自计时，
   `/proc/usbpoll` 第二行 `bootpump:`，启动行 `[C] usbwait`。Pico 2 RV32 SMP：
   - 有主机：72 183 / 1 730 780 次、346 822 µs、**4 804 ns/poll**、mounted；
     暖重启 71 869 次 / 345 340 µs / 4 805 ns/poll，稳定。
   - 跑满界（临时 `usb_cdc_replug(0)` 强制无主机，实验代码已撤）：**1 730 780 /
     1 730 780 次、8 295 302 µs、4 792 ns/poll、bound reached**。
   结论：① 40 000 次按这里单价是 **192 ms**，“预算太大”彻底否掉；② 现有等待其实
   是 **8.3 s**，注释写的“约 1.5 s”错了 5.5 倍（3/260 这个系数假设 0.87 µs/次）；
   ③ 预 tick 比稳态慢 1.8×（4.8 vs 2.7 µs）。11-19 那次挂死**仍未解释**，不得以
   “预算太大”结案。卡片：`docs/acceptance/2026-11-20-pre-tick-usb-pump-cost.md`。

   **2026-11-20 复现尝试（#29 仍不关）**：带符号镜像（`TOS_KEEP_SYMS=1`）、UART0
   刷机前开抓、CDC 节点 + shell 应答双通道看（新工具
   `scripts/rp2350/pump_repro.py`）。① 40 000 次紧泵：`[C] polls=40000/40000,
   us=108677`，CDC/shell 正常，20/20 重启（0.28–0.29 s）；② 完全去掉等待：
   **没有 trap**，UART0 上没有 `mcause=0x3`，CDC/shell 正常，20/20 重启
   （0.18–0.19 s）。差异必须记住：本次板子是 Pico 2 而非 11-19 的 ATK 板，
   11-19 镜像未提交且锁序 bug 今天才修（`695db5b`），GP0 线当时是坏的，
   且本次没做拔电冷启动。按 #29 边界**保留等待**，风险继续挂这里；
   要删等待需先补两块板各 20 次冷启动。卡片：
   `docs/acceptance/2026-11-20-rv32-tight-pump-no-repro.md`。

   **2026-11-20 补人工冷启动（次数以日志为准）**：无等待镜像 + `cold_watch.py`
   （UART0 常开，CDC 节点与 shell 双见证），操作者未数拔插次数，监视器记到
   **30 个 `[A] pre-pll`**（其中 28 次完整进 shell）、
   28 次走到 `tos> `、**0 个 `mcause=`**。两段无 shell 的分别是监视器启动时
   正好赶上刷机首启、以及插拔瞬间 0.25 s 后的第二次复位，都不是 trap。
   至此「当前代码可删等待」的证据只差 ATK 板（未接台）；正式镜像已刷回，
   产品代码未动，等用户决定是否删等待。卡片：
   `docs/acceptance/2026-11-20-rv32-nowait-cold-start-20.md`。

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
   （上面这段是当时的复现计划，已执行但 trap 未再出现；等待现已删除，计划作废。）
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
     显示那 33 秒里 **CDC 设备节点一直在**（芯片没复位）。33.56 s ≈ 2 × 16.78 s 的
     “两个看门狗周期”是巧合：2026-11-20 用一句 `for (;;) ;`（看门狗已 armed、无人
     `wdt_feed()`）直接量过，**一次挂死的清场时间就是 ~33.5 s**，不是 16.78 s。
     结论：这块板子上任何挂死都要等 33 秒才回来，所以“它最后回来了”不是通过判据。
   - 修复：`usb_cdc_flush()` 改成先 `usb_tx_drain()`（自己按正确顺序加锁）、再单独取
     USB 锁做 FIFO flush；另外 `plat_reboot()` 置 `g_rebooting`，`plat_console_flush()`
     在该标志下不走 USB（要复位的机器不该等主机或等一把即将被复位的核握着的锁）。
   - 验证：46 次连续 `reboot` 全部 0.50–0.58 s（修前同口径约一半 34 s）；
     `gate_boot.py -n 10 --cpus 2` **10/10 PASS**（uart0 10/10、cdc 10/10、
     uptime 0.31 s）。卡片：`docs/acceptance/2026-11-20-rv32-reboot-stall.md`。
   - 顺带留下：`.noinit` 环形启动日志（只在上一轮没走到 shell 时打印）、
     `plat_boot_stage()`（`kmain()` 各阶段之间打点，端口可选的弱钩子）、
     `scripts/rp2350/stall_probe.py`。
   - 仍然成立的两条经验：**单槽标记会被下一次启动覆盖**（`WATCHDOG_SCRATCH(3)` 的
     `prev_phase` 往往是恢复启动留下的）；**ROM 自己在用 `WATCHDOG_SCRATCH(4..7)`**，
     自记只能用 0..3 或干脆用 `.noinit`。

5. **ARM（RP2350）USB CDC 长输出截断 —— 已修，待 Pico 2 复验（#46，2026-11-23）**：
   现象是在 ~192 B 处停住、后面全丢（128 B FIFO + 已交给端点的 64 B 包）。根因是
   `tud_task()` 只挂在 idle 钩子上：有任务一直 runnable 时 `sched_idle` 不进，
   CDC 端点不再被重新武装，`tud_cdc_write_available()` 恒为 0，`usb_cdc_putc()`
   逐字节丢。

   修法（`ea2d89d`）：tick 只 `PENDSVSET`；`arm_handle_pendsv()` 在"回到 Thread mode
   前的那一个边界"跑 `board_poll_deferred()`（PendSV 优先级 0xE0，抢不了调度器和
   系统调用，所以没有内核锁、USB IRQ 仍可抢占）。只在**用户帧 + pending-exception
   检查之后 + core 0** 置位。idle 泵保留作兜底。TX FIFO 128 → 512 B：泵跑不进
   产生字节的那个系统调用，512 覆盖树里最大的单次控制台写。

   T2 证据：QEMU arm 上用临时探针（数 IPSR==14，未提交）跑两个并发 spinner，
   ~5 s 内 2534 次 PendSV、每 tick 一次、无 fault，之后 shell 正常。**T3 未做**：
   Pico 2 / ATK 都不在机。上板判据见
   `docs/acceptance/2026-11-23-usb-deferred-pump-ea2d89d.md`：`ls /bin`、`ls -l /`、
   `cat`、`help` 完整；`run bench &` 期间敲键有回显；单核与 SMP 各一次
   `heartbeat 6/6`。若仍截断，先查 PendSV 有没有真被置位（`g_usb_irq_count`、
   `arm_handle_pendsv` 打点），再查 FIFO 512 与 `usb_cdc_putc()` 的 flush 边界。

   仍然不做的：ARM kernel thread（只为 1 ms 泵不划算）、USB IRQ 里跑 `tud_task`
   （历史上第一次中断就死锁）、**无条件**在系统调用内部跑泵（PendSV 低于 SVCall，
   泵本来就进不来）。唯一的例外是写者自己被环满卡住时主动推一趟，见 §5c 与
   `docs/acceptance/2026-11-25-usb-tx-flow-control.md`。

## 5c. USB 控制台“每核 TX 环 + 有界锁”之后，板上待跑的三项（2026-11-25，`694945a`）

状态：**Pico 2 已跑（2026-11-25，`d7a3c8a`）：(a) 部分达标、(b) 原 NOT MET，
按“写路径反压 + 短写重试”修复后 ARM 两镜像已 MET；随后补上 RV32 的 PendSV
等价边界，RV32 两镜像也 MET。ATK-DNRP2350AM 四镜像已板测（2026-11-25，
`cat` 21 200 B 两路逐字节一致、各 3 轮，见本卡“ATK 四镜像板测”一节）。(c) RP2040
已于 2026-11-25 补跑（单核 CDC 30/30、SMP CDC 29/30，详见下方 c 条与
`docs/acceptance/2026-11-25-rp2040-open-items-5260d28.md`）**。Pico 2 实测记录见
`docs/acceptance/2026-11-25-usb-tx-ring-pico2-d7a3c8a.md`
（含修复后复测与 ATK 两节）。

**ATK 板测顺带修掉一个面板镜像的挂死**（`kernel/screen.c` `putc_locked()` 的
TAB 展开：光标在最后一列时 `stop == cols`，换行把 `cx` 归零后条件仍成立 →
无限重画，控制台静默、看门狗 ~33 s 复位；只在面板镜像开启且输出里出现
“行尾 TAB”时触发）。修法是按格数计数而不是与目标列比较。这条只在
ATK（有 ST7789 面板）上可见，Pico 2 没有面板、该分支不可达，所以 Pico 2
不用重跑。别再把 `[S] site=` 的最后一条当成“卡在这里”的直接证据：每画一格
96 B 就有数百条探针，16 条环里永远只剩最后一次写。

**已在 Pico 2 上测到的结论（先看这里，别重复跑）**：四镜像重建 4/4；
`cat` 21 200 B 文本经 CDC 的结果是 ARM 单核 0/3 完整（只出 255–271 B）、
ARM SMP 3/6 完整（失败时 276 B）、RV32 单核 0/3 完整（1 216 B）、
RV32 SMP 3/3 完整；UART0 四条都完整。计数器：`N = 0`、`U = 0`，
但 `D` 随失败的 `cat` 等量增长（3 次失败 ≈ 64 KB）。改动前的 `0a30b69`
每次出 576–581 B，所以不是本轮引入，但 (b) 的目标没达成。RV32 用的是
`drivers/usb/rp2350rv`，不在 `694945a` 的改动范围内，它的计数恒 0 是
“没有这套计数器”。另：`/proc/usbboot` 自己 330 B 的输出在 CDC 上经常丢
最后一行，计数要从 UART0 读。

**修复后复测（2026-11-25，本提交；四镜像 + 四个 RV32 目标重建通过）**：
`cat` 21 200 B 经 CDC 的结果是 ARM 单核 9/9、ARM SMP 3/3、RV32 单核 9/9、
RV32 SMP 9/9 全部完整（均为 21 621 B、首尾行与提示符齐全）。四条镜像 CDC 口
关闭时 heartbeat 仍 6/6；把 CDC 口打开但不读（Windows 侧缓冲写满）后，UART0 上
的 `cat` 仍完整、提示符 2.19 s 回来，250 ms 截止路径不会把 shell 卡死。
做法是三层 + 一个 RV32 边界：板级 `plat_console_can_take()` 报 CDC 余量
（默认弱符号恒 1）、内核 `console_write_some()` 满时短写并在 250 ms 后转为
“照旧丢弃”、用户库 `write_all()` 把 0 字节当作背压并 `yield()` 重试；RV32 因为
没有 PendSV，在 `arch/rv32` 的 tick 返回用户态处补了一个等价边界（开中断调用
`board_poll_deferred()`，tick 仍是 1 kHz、时间不漂）。细节与逐轮数据见验收卡
“方案 1”与“方案 1 扩展到 RV32”两节。

这一轮把写方向也搬出了锁（`putc()` 只入本核 256 B 环，
泵在锁内搬进 TinyUSB），并把“抢门超时 → 永久无锁”改成“计数 + 跳过本次操作”，
RP2040 的 `usb_cdc_lock()` 也从无界自旋改成有界获取。上游侧只做了 T0/T1/T2（18 目标、
arm QEMU smoke、soak 2/2）与构建；板级由 Pico 2 先跑（结果见上），ATK 与 RP2040
仍未跑。背景、代价与验证清单：
`docs/acceptance/2026-11-25-usb-tx-ring-bounded-lock.md`。

**方案 A 已撤回（`ba657e4` → 本提交）**：曾让环满时的写者自己循环调
`usb_cdc_poll()` 等主机。撤回的理由是它违反 `kernel/plat/console.h` 的契约
（“putc may drop … blocking on a peer would stall every other core's output”），
而且把“谁推进泵”塞回了写路径。

**方案 B（已实施）：`write` 如实返回短计数** —— `putc` 环满即失败，
`console_write()` 在第一个失败处停下并返回实际接受的用户字节数，`process_write()`
的 `FD_CONSOLE` 返回它，用户态 `write_all()` 重试（`n == 0` 时 `yield()` 后重试，
**不能把 0 当终止**）。`cat` 的循环会被 tick 抢占，泵每 1 ms 排空，最终写完。
计数必须精确到 `plat_console_putc()` 的返回值，不能用 `plat_console_tx_room()`
预判（SMP 会竞争；“没枚举”要区别于“环满”）。已知边界：`kprintf`、tick echo、
fault/panic 没有重试语义，仍按契约丢。(b) 的判据因此是“**短计数 + 重试后完整**”。
细节：`docs/acceptance/2026-11-25-usb-tx-flow-control.md`。
RV32 的 `drivers/usb/rp2350rv`（1 KiB 环、泵只在 CPU 0）用同一机制即可，
它的 `usb_cdc_tx_room()` 要从“tud 空闲”改成“环空闲”。

**实际落地的是“预判 + 短写”，不是改 `putc` 的返回值**：新增板级弱钩子
`plat_console_can_take(need)`（默认 1；ARM 报本核环余量，RV32 报 TinyUSB FIFO
余量；未枚举时恒 1，所以不会把“没主机”误判成“满”），`console_write_some()`
在钩子说不行时停下并返回已写字节数，250 ms 拿不到空间就转回“照旧丢弃”，
`write_all()` 对 0 字节 `yield()` 重试。这样不用改 18 个目标的 `putc` 签名；
代价是同核的 tick echo 在检查与 `putc` 之间抢进环时，那一个字节仍会丢（不是截断，
且 echo 本来就是可丢的）。RV32 的 `usb_cdc_tx_room()` 语义没动，预判用 FIFO 余量
（比环更紧，生产者不会走到环的丢弃阈值）。

三项都请走 **CDC 控制台**（UART0 上那一路不经过这段代码，测了不算数）。

- **a. 计数器**：跑完下面 b 的长输出，以及一次
  `run prot heap`（等 4 s）+ `deep 3`（等 8 s）之后，`cat /proc/usbboot`。
  新增的最后两行是
  `lock timeouts N  ungated=U` 和 `console tx dropped=D  handed=H`。
  **判据**：`N = 0`、`U = 0`，长输出后 `D` 很小或为 0（短计数 + 重试生效时不该
  有大量丢弃），`H` 随输出字节数增长。`694945a` 实测：`N=U=0` 但 `D +64 263`。
  `U = 1` 说明这块板的 SIO 三个候选门一个都没拿到（只有 boot 期探测失败才会如此，
  等于整个启动无锁运行）；`N > 0` 说明确实有路径在锁上卡了 ~20 ms，把当时的
  命令和前后行一起记下来——那表示还有一条没人想到的长临界区。
- **b. #46 回归（单次 >16 KiB 输出不截断）**：`cat` 一个 ≥16 KiB 的文件（或
  `ls -l /bin` 反复拼接），比较行数/字节数与源文件。**判据**：完整、结尾有换行、
  中途没有空洞。注意第 5 条已定的边界仍在：单次 >512 B 的系统调用写是有界丢字节，
  这轮没有改变它；验的是“连续输出不再在 ~192 B 停住”。
- **c. RP2040 —— 2026-11-25 已跑（`5260d28`）**：两镜像 `accept.py` 见 §2c；
  `cat /proc/usbboot` 重插后基线 **N=0 U=0 D=0 H=758**（(a) 判据 PASS）。gap 2→1 ms
  实验：42 KiB / 128 KiB 各 3 次，**NAK 全 0、CRC 全通过，但 D 从 836 涨到 7742**
  （42 KiB ≈+710/次、128 KiB ≈+1428/次）——按判读是**环满（时序不够）**，D=0 未达成，
  说明 2 ms gap 是必要值；实验值已用 finally 恢复为 2 ms、未提交。SMP CDC 的
  `fault isolation` FAIL 与 gap 无关（UART0 3 轮证明板级 fault 正常）。细节与逐轮数据见
  `docs/acceptance/2026-11-25-rp2040-open-items-5260d28.md`。

  **决定（2026-11-25）：RP2040 的 fault / panic 报告只走 UART0。** 新增 weak 钩子
  `plat_console_mirror(on)`（`kernel/plat/console.h`；`panic()` 与 `arch/arm/platform.c`
  的两处转储在打印期间调用），`board/rpi-pico` 实现为"置位后 `plat_console_putc()`
  不再镜像到 CDC"。理由就是上面这段：M0+ 的 HardFault 高于所有 IRQ，dump 期间
  USB 完成中断和 tick 泵都进不来，镜像只能收到半份报告。`scripts/rp2040/accept.py`
  在 CDC 控制台上把 `fault isolation` 记为 **skipped**（仍要求提示符返回、无 panic），
  UART0 行保持严格检查。CDC 环回到 **256 B**，背压改由板级 `plat_console_can_take()`
  提供（与 RP2350 同款：环满 → 短计数 → `write_all` 让出 → tick 泵）；按文件大小
  定环不可持续。
  卡片：`docs/acceptance/2026-11-25-rp2040-fault-uart0.md`。

  **2026-11-25 定位更新（两个候选补丁均已撤回，修复未落地）**：在 ATK-DNRP2350AM ARM SMP
  上同时打开 UART0（COM14）与板载 CDC（COM26，DTR=True）抓同一次 `crash`，UART0
  997 B 完整、CDC 267–311 B，首差在 262/306 B，正是 256 B per-core TX ring 写穿；
  `D` 增量与丢失字节数相等。 RP2350 的 fault 优先级可降到 0x40，USB IRQ(0x00) 与
  SysTick(0x20) 能抢占，所以 prompt 会在 dump 被截断后补出来；M0+ HardFault 固定在
  所有 IRQ 之上，整段 dump 期间 pump 抢不进来，prompt 也会落进满环。UART0 不丢不是
  因为环大：硬件 FIFO 只有 16 B，`plat_console_putc()` 每字节等 `FR.TXFF` 清零，
  由 UART 控制器自行按波特率发送，不依赖中断/pump。
  曾试在 `arch/arm/platform.c` 的 `process_exit()` 前调用 `plat_console_flush()`：
  ATK ARM SMP 无回退，但 RP2040 SMP 实测 **ACCEPT FAIL 25/30**，`crash` 后 30 s
  CDC 无 prompt，已撤回。第二个方案是在 `board/rpi-pico/platform.c` 的
  `plat_console_putc()` 里读 `IPSR`，仅在 `IPSR != 3` 时写 CDC（M0+ dump 只走
  UART0）：刷入后单连接探针里第一次 `crash` 只回 41 B（停在
  `about to execute an illegal instru`），第二次 0 B，CDC 之后完全沉默；UART0
  同一时刻仍是完整 HardFault dump + prompt。说明按异常号判别会把正常线程态输出
  也一起挡掉，已撤回该补丁并重建基线镜像。当前 `fault isolation` 仍按失败记录，
  修复未落地。

若 (a) 出现 `N > 0` 或 (b) 仍截断，先看 `/proc/usbboot` 的 `handed` 是否还在增长：
增长说明泵在工作、问题在主机/端点；停住说明泵没被调度，回到 §5b 第 5 条的 PendSV 检查。

## 5d. ARM 异常优先级/向量号补丁（`87ede23`）的板上待跑两项（2026-11-25）

状态：**a 已 PASS（2026-11-25），b 仍 NOT RUN**。补丁内容：SHPR1 的 `[31:24]` 在
ARMv8-M 上是 SecureFault（不是保留字节），也按 `ARM_PRIO_FAULT` 写；`arm_default_handler`
改成 `mrs r1, ipsr`，不再把所有非常规向量都报成 `vec 0`。本机 T0/T1/T2 与 6 个 RP2
目标 + `arm`(M3) 构建 PASS。背景与推导：
`docs/acceptance/2026-11-24-arm-securefault-priority.md`。

- **a. Pico 2 ARM（M33）——已 PASS（2026-11-25，`d7a3c8a`）**：板上重跑 `0a30b69`
  （含 `87ede23`），两块 ARM 镜像的故障转储都读到 `shpr1=0x40404040`（SecureFault
  字节由 `0x00` 变 `0x40`，MemManage 仍在 `bits[7:0]`）；`rpi-pico2` heartbeat 6/6 +
  五例 `prot` 全 MemManage，`rpi-pico2-smp` heartbeat 6/6、smptest 11/11、dd 50/50、
  smpbench 2.05x。父提交 `05a0e18` 上同一行是 `0x00404040`，见
  `docs/acceptance/2026-11-24-pico2-matrix-05a0e18.md` 注 5。ATK 板不在场，ARM hammer
  未重跑（不是关闭门槛，保持 `NOT RUN`）。
- **b. RP2040（M0+）—— 2026-11-25 已跑（`5260d28`，见 §2c 与验收卡）**：`rpi-pico`
  UART0 + CDC 两路 `ACCEPT OK 30/30`（含 `fault isolation`）；`rpi-pico-smp` UART0
  `ACCEPT OK 30/30`（含 `fault isolation`）。**SMP CDC 一行 29/30**：`fault isolation`
  FAIL（crash 后 10 s 没读到结尾提示符），但同板 UART0 连续 3 轮 crash 都正常打印
  `[pid N] HardFault at 0x2001409a`、回提示符、uptime 366→370→373→376 s 不复位
  (`prompt_in_crash=True, panic=False`)。板级 `arm_default_handler` 的 IPSR 读取在 M0+
  上生效无疑；CDC 那一项判为**串口/输出时序问题，不是 fault 逻辑**，单列待查，
  不阻塞本补丁的板级结论。该输出时序项的根因与待复测补丁见 §5c(c) 的 2026-11-25
  定位更新。

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

## 12. rcc / RV32 原生路径的能力边界（2026-11-21）

RV32 原生链路 `csc -> rvm -> rcc -> ELF` 已在 QEMU rv32 和 C3 上跑通 40 个程序
用例（`docs/acceptance/2026-11-21-node-hash-rv32-8d15cbc.md`，前几轮见
`2026-11-21-node-256-rv32-945fbe0.md`、
`2026-11-21-func-table-rv32-b461b91.md` 和
`2026-11-20-rv32-native-suite-rv32-0ae04d7.md`）。下面几条是**已知边界**，不是
回归；写下来免得下次当成 bug 重新查。

1. **`csc`：256 个函数、单函数 256 个 AST 节点。** `src/csc.c` 用紧凑签名表
   （`Sig` 20 B + 名字池）取代了 340 B 的 `Func`，板上 256 个函数约 11 KiB，和
   以前 32 个函数一样大；`NODE_MAX` 从 128 提到 256，但 `Node` 同时从 124 B
   压到 **56 B**（标识符只存 64 位 FNV 哈希，声明名和字段路径放每个函数重置的
   3 KiB 文本池），所以 `csc` 窗口只从 127 KiB 涨到 **131072 B（128 KiB）**。
   现在 `many25`/`many32`/`bigcode`/`bigir` 都能在板上
   `csc -> rcc -> exec`。仍编不过的是**更长的单函数**：`bigbody` 有 90 条语句、
   约 450 个节点，需要 `NODE_MAX` 512；这类用例继续由宿主 `csc` 生成 RIR2。

2. **`rcc` 的板上上限是约 2000 IR。** C3/rv32 现在是 RIR2 ≤ 40 KiB、`PC_MAX`
   2048、4096 个标签（实测约 1.45 标签/IR）、text ≤ 48 KiB、256 个函数。RIR2
   不再整文件驻留：指令区用 4 KiB 滑动块读，函数表（≤1 KiB）、字符串池（1 KiB）、
   arena（512 B）各读一次；`RIR_MAX` 只作为输入大小校验保留。板上 BSS 从 71.9 KiB
   降到 37.6 KiB，窗口 124 → **88 KiB**，宿主编译时间多约 2%。宿主仍放宽到
   RIR2 256 KiB、16384 PC/标签、1024 个函数，`hugeir`（2948 IR）只在这里跑。再往上
   （8000+ IR）要把标签改稀疏、放宽 `PC_MAX`/`LABEL_MAX`；`label_off`/`label_seen`/
   `pc_label` 收窄到 u16/位图还能再省约 15 KiB。

3. **`rvm` 不再有整程序 IR 上限。** 指令区改成按页读取（`PAGE_IR` 256 条，
   一次 5120 B；宿主 8 页、板上 2 页），`ir[]` 不再整块驻留，`IRMAX` 只做头部
   校验（65536 条）。`bigcode`（535 IR）、`many100`（898 IR）、`bigir`（1436 IR）
   和 `hugeir`（2948 IR）因此从 native-only 转成普通用例：宿主同时校验解释器和
   原生镜像。板上只有 2 页，跳转到 512 条以外的代码会换页，只是变慢，结果不变。

4. **qemu-virt ramdisk 只有 512 KiB。** 40 个 `.tos` 约 200 KiB，整套一次装不下，
   冒烟要分批；`files.local/` 只用于本地暂存，不提交。

5. **RP2350 Hazard3 的 T3 仍是 NOT RUN。** C3 已跑 `many100`（板上 `csc` 111 个
   函数 + `rcc` 1208 标签）、`many32`、`bigcode`（板上编译，535 IR / 780 标签）和
   `bigir`（板上编译，1436 IR / 2062 标签）；RP2350 上同一批 RV32IM 镜像尚未上板。
   按 2026-11-19 的硬件策略，它属于关闭条件的一部分，等板子排期。

6. **RP2040 板内链路已验收（2026-11-21）。** `csc -> rcc -> exec` 三例（整数/字符串/
   浮点）在 M0+ 上跑通，输出与 `rvm` 逐行一致；窗口实测 `csc` 128 KiB、`rcc`
   100 KiB、`rvm` 44 KiB，而用户区只有 216 KiB——`csc`+`rcc` 加起来 228 KiB 装不下，
   所以三个工具只能作为文件分时运行，这就是 `configs/rpi-pico-rir.conf` 的取舍依据。
   工具是 `scripts/elfwin.py`（#39 把它列为证据，但此前两仓都没有，本轮补上）。
   卡片：`docs/acceptance/2026-11-21-rir-rp2040-f466efa.md`。

## 13. ARM Thumb-2：stage 2 完成，Thumb-1 与 RP2350 仍待做（2026-11-21）

`rcc` 按架构选择后端（`rv32.inc` / `thumb2.inc`）。Thumb-2 已在 QEMU
`arm`（mps2-an385, Cortex-M3）跑通 28 个用例，覆盖整数、字符串、Q16.16
float 和 native 系统调用；输出与 `rvm` 一致：

- 寄存器：RIR 0..6 在 r4..r10，RIR 7..15 在帧槽；参数走 `data_argbuf`，
  返回值 r0，helper 只用 r0..r3/r12 并保存 r4-r11。
- 地址不依赖重定位：`e_la` 用 `bl .+4` 取 PC 再减链接期偏移，镜像仍是两个
  PT_LOAD、没有 PT_DYNAMIC。
- 数据段按 32 字节对齐（MPU 最小区域），否则 arena 头几个字落在只读代码段。
- 循环计数用 `SUBS`（Thumb 的 `SUB` 不设标志），`print(float)` 去尾零、
  `printf("%.Nf")` 保留 N 位。

**还没做**：

1. ~~RP2350（Cortex-M33）真机 `NOT RUN`，目前只有 QEMU 的 M3~~ → **已验（2026-11-21）**：
   Pico 2 上 `rpi-pico2-smp` + 44 个 Thumb-2 语料，心跳 6/6、`strict 37/37` 两轮，
   卡片 `docs/acceptance/2026-11-21-thumb2-rp2350-ae8c7c7.md`。~~单核 ARM 目标带语料时
   到不了 USB 枚举~~ → **已修（2026-11-21，`5557f33`）**：QMI 探测写 `DIRECT_CSR` 时把
   CLKDIV 清成了 0（SCK=150 MHz，芯片不回应），有界等待跑满 1e6 次把启动拖过 2.1 s
   看门狗；保留分频 + 快速失败后单核能启动并枚举。卡片
   `docs/acceptance/2026-11-21-pico2-uni-arm-boot-5557f33.md`。单核还剩一条**控制台输出
   约 192 字节截断**（#46），与启动无关。
   **勘误（2026-11-22，HISTORY §105）**：修复有效，但上面的机制写错了——实测探测只花
   47 µs（SMP）/108 µs（单核旧参数），探测期间看门狗一次都没被喂（SMP `probe_dbg`
   in=out=1），所以不是"等待跑满 1e6 把启动拖过 2.1 s"。单核用旧参数（CLKDIV=0、bound=1e6、
   无阶段喂狗）今天仍能启动；只有把**退出时写 0** 那一行恢复，才稳定复现失败。真正的
   要害是探测退出时把 CLKDIV 清成 0，XIP 恢复后 flash 时钟分频丢失；SMP 没有这个现象
   是另一条尚未查明的差异，不是"有人替它喂狗"。**再勘误（§106）**：RV32 单核
   （`rpi-pico2-rv32`）心跳 6/6，而它的探测代码与修复前 ARM 逐行同构（进入写
   EN|ASSERT、退出写 0、bound=qmi_spin_limit），XIP 恢复函数也相同——所以
   "退出写 0 是要害"也不成立。目前只能说这是 **ARM 单核专有**，与探测写法
   不是简单因果；
2. native 的 GPIO/ADC/PWM 只有编译覆盖，没有外设实测；
3. QEMU mps2 的 600 KiB ramdisk 装不下整套用例，冒烟要分批。

**三个后端同语料（2026-11-21）**：同一套 44 个程序现在在 Thumb-1/RP2040（#38）、
Thumb-2/M33（#40）、RV32/Hazard3（`docs/acceptance/2026-11-21-corpus-rv32-rp2350-67a70ea.md`）
上都跑过 `strict 37/37`。RV32 端不需要另写运行器——`thumb1-prep.py --arch rv32` 用
`RCC_ARCH_RV32` 后端出镜像，期望输出仍来自宿主 `rvm`，所以判据和金标文件完全一样。

## 13b. ARMv6-M（Thumb-1）：后端完成，等 RP2040 上板（2026-11-21）

`thumb1.inc` 覆盖了 Thumb-2 的全部 RIR 操作（整数、字符串、Q16.16 float、
native、`capture`），47 个语料中除 2 个故意编译失败的用例，其余在 QEMU mps2
（Cortex-M3 执行 v6-M 子集）上全部与 `rvm` 输出一致；表驱动反汇编检查确认
48 个镜像的代码段只含 ARMv6-M 编码。RP2040 没有 QEMU 模型，真机仍是 `NOT RUN`。

与 Thumb-2 的差别：

- 只有 r0..r7 能作基址和 ALU 操作数，所以 RIR 0..3 在 r4..r7，RIR 4..15 在帧槽；
- 没有 `movw/movt`：常量用 `movs/lsls/adds` 逐字节拼，`e_la` 固定 7 条指令并
  保存 r2/r3（helper 会把状态放在 r3）；
- 条件跳转只有 ±256 B，长分支一律 `b<inv> +4; bl target`；只有“跳回 helper 入口”
  那条必须用真正的 2 字节 `B`（BL 会覆盖被调方刚保存的返回地址）；
- 没有 `udiv/sdiv/mls/smull`：软件除法（32 次移位减法）和 16×16 分块乘法；
- 窗口约 90 KiB（与 rv32 相同量级）。

**RP2040 上板：已通过（2026-11-21 16:18，撤回解除）。** 当天 15:39 曾以"板上
HardFault"撤回 `7a25c54` 的卡片；16:14 按原样重建得到**逐字节相同**的镜像
（sha256 `39c9ae20…`），重刷后心跳 6/6、`thumb1-run.py --set all` 连续两轮
`strict 37/37`、7 个观察项无 fault，撤回解除（卡片顶部有复现记录）。

撤回时看到的 fault 另有来源：当时板上是 #39 的 rir 测试镜像。`tos build` 那时只传
`-mcpu`，`rcc.c` 对任何 `__arm__` 都落 `thumb2.inc`，M0+ 镜像里装着一个 Thumb-2
代码生成器，它产出的程序在 `0x2001000a`（窗口首条指令）就 fault——即 HISTORY §97，
已由远端 `7c51dae`/`451fa4d` 修掉（`[target] rcc_arch`，配置期拒绝 thumb2 配
ARMv6-M）。44 个镜像本身经 `v6mcheck.py` 复核为纯 v6-M（`OK (44 image(s))`）。

**板上工具链可以装下（2026-11-21 实测，M0+ 构建）：** `configs/rpi-pico-rir.conf`
把 csc/rvm/rcc 放进同一个镜像，窗口分别是 128 / 44 / 100 KiB。三者不同时运行，
峰值 128 KiB，RP2040 的 256 KiB SRAM 够用；`__aeabi_lmul` 由 M0+ 的 libgcc 提供。
C3 上实测 `csc` 2.4 KB 源 140 ms、`rcc` 95 IR 130 ms，44 个程序一轮约 30–60 秒，
RP2040 大约翻倍。因此测试可以只 stage 84 KiB 的 `.c`，在板上依次
`csc -> rvm -> rcc -> exec`，一次覆盖三个阶段（`thumb1-prep.py --source` +
`thumb1-run.py --source`）。

镜像大小不再需要改 `ramdisk_sectors`：MKFS 装不下时构建会把 ramdisk 翻倍重试，
直到板子的分区上限（仅 `fs_backing = flash`；disk/embedded 保持固定，因为镜像要
原样进 RAM）。所以 44 个测试镜像可以直接 stage，RP2040 和 Pico 2 都不用手改板子。

**2026-11-24 追加：这条当时漏了 ATK 的 RISC-V 半边。** `0b5e224` 只改了
`board/rp2350rv-generic` 的 conf 与 layout，`board/atk-dnr2350am-rv32` 仍写着
`grain = 4k` 且没有 `stack_align`——它继承的 `layout.c` 却是 16 KiB，于是
`board_config_check()` 在 `kmain` 里直接 panic。没有 QEMU 模型、ATK 又不是门槛，
所以一直没人看见。现在 ATK 的 conf 是 16 KiB + `stack_align = 1`，并且
`internal/build/layoutcheck.go` 在**构建期**比较 conf 与要链接的 `layout.c` 的三个
数（grain/stack/guard），不一致直接拒绝构建，不用等上板。

**RP2350 RV32 的窗口粒度是 16 KiB，不是 4 KiB。** PMP 端口把栈放在独立的
NAPOT 区域，`stack_request_ok()` 要求栈提示整除 `PLAT_APP_GRAIN`，而板上工具
（csc/rcc）要 16 KiB 栈。所以 `board/rp2350rv-generic/layout.c` 覆盖 ARM 基线
的 4 KiB，用 16 KiB（24 个窗口）；`board.conf` 里配 `stack_align = 1`，构建把它
生成成 `TOS_APP_STACK_ALIGN`，内核钩子读同一个值。`tos build` 通过
`mklfs --grain` 在打包时校验每个程序的栈提示，不合格直接报程序名——以前只有内核
在 exec 时才拒绝（#44）。ARM 半边用 MPU，保持 4 KiB。

**NAPOT 块预算不够时逐级降级，而不是每次调度刷一行。** Hazard3 只有 8 个软件
可写 region，其中一个（entry 5）固定给 trap vector，所以窗口只有 7 块可用。
16 KiB 粒度下，页粒度的 code/data 切分可能把窗口拆成 8 块：实测 rcc 的 96 KiB
窗口（wx 在 +28 KiB）在**一半的 16 KiB 对齐基址**上需要 3 code + 4 data + 1
stack = 8 块——所以失败看起来是随机的，取决于 `win_alloc` 把窗口放在哪。旧代码
会带着半套 PMP 继续跑，并在每次任务切换时打印
`[pmp] window needs more NAPOT blocks...`。现在
`board/rp2350rv-generic/pmp_map.h` 的 `pmp_map()` 按保护强度依次尝试：页粒度
切分 → 按粒度对齐切分 → 整个 image RWX → 整个窗口 RWX。只有最后一级（会盖住
guard hole，`prot heap`/`prot stack` 不再触发）在开机时提示一次；四级都放不下
时才把用户内存全部关掉并只报一次。算法由 `internal/build/pmpmap_test.go` 在
宿主机上编译真实头文件、枚举 32–192 KiB 的所有页粒度切分来验证，并固定了 rcc
（必须降级）和 csc（必须保持完整保护）两组实测几何。

**取指跨两个 PMP 条目的规则（2026-11-24）。** RISC-V 规定一次访问只有在**单个**条目
覆盖它时才算匹配，所以一次取指横跨两个条目（即使两边都给 X）必须 fault。Hazard3 的
映射是页粒度 NAPOT：一段 `.text` 会被 `pmp_cover()` 拆成几块，块边界就在页边界上。
带 RVC 时 32 位指令可以落在 `page_end-2`，一条取指覆盖两个条目，于是 `rcc` 在
`0x20026ffe` 报 `instruction access fault`（同一份源码挪 0x30 字节，崩点跟着变成
`0x20025ffe`）。修法是一条**板级策略**，不是 build system 里的板级分支：
`board/rp2350rv-generic`、`rpi-pico2-rv32`、`atk-dnr2350am-rv32` 的 `board.conf` 写
`[user] rvc = auto`，意思是“先按 target 的 ISA（带 C）编，哪个程序链接出来会跨页，就只
把那一个程序换成不带 C 重新链接”。新加一块同样的板子只加这一行；Go 里没有任何板名/芯片
名。`rvc = no` 则是整镜像不用 C，`rvc` 不写就是今天的默认（允许 C）。

落点是 `internal/build/build.go` 的 `compileUserTree()`：默认树在 `build/user/`；链接
完全部程序后，用 `checkCodePages()` 的同一个指令遍历器读镜像，只把**跨界的那几个**程序
（连同公共运行库的一份）编到 `build/user/noc/` 并重链一次（4 字节指令不可能跨界，一遍
即够），日志打印 `RVC bin/xxx: rebuilt without compressed instructions`。
`buildFSImage()` 里的同一个检查是最后的门禁。

**边界按页算，不按 grain 算。** 曾经以为“块边界只落在 grain 的倍数上”，于是把检查收紧
到只看 16 KiB 线——**这是错的，而且正好放过了 rcc**：RX run 的结尾是
`page_down(.data 起点)`（那一页因为不能拆，整页变成 RWX），这个边界是**页**的倍数，
不一定是 grain 的倍数。实测 rcc 的 code run 是 0x7000（28 KiB），崩点
`0x20026ffe` 就是 28 KiB−2，正是这个边界。所以检查器看每一个 4 KiB 页边界（保守：
有些页边界落在同一个 NAPOT 块内部，本来不会 fault；宁可多回退一个程序，也不漏掉
rcc 这种）。

实测（16 KiB 粒度的 `rpi-pico2-rv32` / `-rv32-smp` / `rp2350rv-generic`，与 4 KiB
粒度同样）：`edit pio sh csc rcc rvm` 六个程序被换成 no-RVC；`msh` 的 `code run`
只有 4 KiB，没有内部页边界，保留 C。

所有情况下 `msh` 都保留 C（窗口 **16 KiB**），内核也保留 C。只有被换掉的
程序变大（同一份源码 no-RVC 比带 C 大 +23%…+47%）。

**这条上游已经改过（2026-11-24 补记）。** Hazard3 **v1.1** 的 release notes，
“General Improvements” 里写着：

> Relax PMP X permission checking to allow instructions to straddle two PMP regions
> which both have the necessary permissions.

同版本还把取指权限查表从“两个半字”改成“自然对齐的取指字”（PPA optimisations）。
也就是说“跨条目必 fault”是 **v1.1 之前的实现选择**：规范允许实现更严格，但上游自己
把它放宽了，所以按“坑/bug”写是有依据的。RP2350 用的是更早的版本（v1.0.1 的 notes 里
“It is not present on the Hazard3 version used on RP2350” 也印证了这一点），所以这块板
仍然需要 `[user] rvc = auto`；将来换到 v1.1 及以后的实例，那一行可以直接去掉，压缩
指令和体积都回来——策略是板级事实，不需要改构建器。

硬件依据（不要再猜）：Hazard3 的 `PMP_GRAIN` 是 RISC-V 的 G 参数，**最小 region =
1 << (G + 2)**，G > 0 时 NA4 写入会被强制成 OFF；RP2350 的实例化是 **G = 3**，即
**32 字节最小粒度**、NAPOT only（TOR 关）、8 个可写 region（pico-sdk
`runtime_init_stack_guard.c` 的注释与 RP2350 datasheet §3.8.3 一致）。所以：

- 4 字节粒度**不可能**——不用再上板复验；`pmp_map.h` 的旧注释“sub-page NAPOT 不
  生效”其实说的就是这条，已改成引用 G = 3。
- 32 字节的“桥接条目”是可能的：在跨界的 32 字节 chunk（`[boundary-16,
  boundary+16)`，天然 32 字节对齐）上放一个小 NAPOT RX region，跨界的指令就整个
  落在它里面。代价是每个出问题的边界多占一个条目，而预算只有 7 个（rcc 本来就要
  降级），所以只是**备选**，不替代去 RVC。
- 另一个思路是让 code run 整个落在一个自然对齐的 2^k 块里（没有再切分就没有边界），
  但只对小程序成立（把 code 补到 16 KiB 边界）。

这条不再靠上板才发现：`internal/build/codepage.go` 的 `checkCodePages()` 在打包用户
镜像时逐条走指令流（低两位 0b11 = 4 字节，否则 2 字节），任何指令跨 4 KiB 边界就
直接**构建失败**并打印地址。门限是那条板级策略：`[user] rvc` 非空（`auto`/`no`）的
rv32 目标才检查。TOR 端口（QEMU/C3）没有这条策略，它们的映射一段只有一个条目，跨页
无害，所以不检查、也不因此变大。
负例：往 `user/<prog>/opt` 里塞 `-march=rv32imac`（opt 追加在命令行最后）时，
`bin/rcc` 在 `0x00000ffe` 被拦下；把门限临时放开到 QEMU 时 `bin/edit` 同样被拦。

RP2040 刷写走 BOOTSEL；仓库里没有自动化 UF2 流程，`scripts/rp2040/autoflash.py`
只负责等待盘符并拷贝。板级验收现在是一条命令：
`scripts/rp2040/accept.py --uf2 build/tinyos-rpi-pico.uf2`（等 BOOTSEL → 刷机 →
身份/时钟/堆/文件系统/exec/写回/fault 隔离/长输出/两轮 dd+rm 全跑，末行
`ACCEPT OK n/n`）。2026-11-23 起 RP2040 重新是 T3 必跑项（`docs/TESTING.md`
§5.3），长输出那一项要走板载 CDC——UART0 覆盖不到 USB 路径（#46 就是那条）。

## 14. 用户态堆：first-fit 空闲链表（2026-11-21）

`user/heap.c` 取代了原来的 bump 分配器：`malloc` 先扫空闲链表找可复用的块并切出
需要的大小，`free` 标记后与地址相邻的空闲块合并，只有整张链表都不够时才 `sbrk`。
形状与 FreeRTOS `heap_4` 相同。

已知边界，暂不打算在本轮补：

- **不收缩 `brk`**：`free` 让内存可复用，但不还给窗口，所以栈不能长进已释放的
  堆尾。这样 `sbrk` 只增不减，和 `kthread`、`deep` 的栈增长路径都不冲突。
- **没有 `realloc` / `calloc`**；需要时在 `heap.c` 上补。
- **不是线程安全的**：kthreads 共享组窗口，两个线程同时 malloc 会竞争；和原来的
  bump 指针一样。`kthread_create` 只在创建时分配，运行中的线程各自 malloc 才需要
  锁。
- 块头 16 字节（32 位目标），所以 `TOS_HEAP(n)` + 一次 malloc(n) 需要 n+16 的区域；
  `bench`/`wheap`/`mem` 已各留 64 字节。声明多个块时每块再占一个头。

固定块池（`mempool` 形状）仍然是确定性要求高的程序的更好选择；这里只把“free 是
空操作”这个最容易踩的坑去掉。

`configs/arm-rir.conf` 是 test-only 镜像（`rir = true`、`scripting = false`），
任何发布配置都不要打开它。

## 15. pinned 工具链副本单侧同步（2026-11-22，**已解决**）

> **状态：2026-11-22 已修复。** 双方 pinned 源已同步完毕（TinyOS 侧到
> `2731bcb`，`tinyos-script` 侧到 `04815b6`）；当前
> `go test -count=1 ./...` 全绿，无需 `-skip`。以下保留问题当时的记录与
> 复现方法，供以后两侧再次漂移时参照。

`76408d1` 把 div 语义与 spill guard 的改动同步到了 TinyOS 侧的
`user/{csc,rvm,rcc}`，但没有同步 `../tinyos-script/src/` 对应的四份源文件。
在有 `../tinyos-script` 检出的工作区里，`TestPinnedToolchainSources` 因此从
`76408d1` 起在 T0 红：

```
pinned_test.go:44: user/csc/csc.c differs from tinyos-script/src/csc.c;
copy it across and commit both
```

**限定条件**：该测试在找不到 `../tinyos-script` 时是 Skip（`pinned_test.go:23`），
所以 CI 与单独 checkout 的 TinyOS 仍然是绿的；只有同时检出两个仓库的开发机上会红。
这不是"CI 该红却绿"，而是本地一致性检查只在两棵树都在时生效。

在干净 worktree（HEAD = `76408d1`，无任何本地改动，且兄弟仓库存在）上已复现
同一条失败，确认不是别的改动引入的。`go test -count=1 -skip
TestPinnedToolchainSources ./...` 全绿，所以影响面仅限这一对一致性检查。

修法：把 TinyOS 侧的四份改动同步回 `tinyos-script/src/`，两边各提交一次；
不要为了让 T0 变绿而删除或放宽 `pinned_test.go`。**已按此完成**（2026-11-22）。
以后若两侧再次漂移，本地 T0 报告要显式写出这条 FAIL 及基线证据，不能写成"T0 全绿"。


## 16. 结构性限制清单（2026-11-23）

和“用户堆只增不减”同类的东西：不报错、不崩溃，用着用着才发现。按影响排序，
每条给出证据位置；修不修、什么时候修由需求决定。

| # | 限制 | 证据 | 表现 | 建议 |
|---|---|---|---|---|
| 16.1 | **任务窗口不可压缩**：每个窗口必须连续，释放只留空洞 | `kernel/process.c:1384`（`win_alloc` 扫空隙，无搬迁） | 混合大小的 exec churn 后，大窗口（C3 上 `csc` 要 128 KiB）可能在“总空闲够”时仍失败；退出任务或重启才恢复。错误会报最大空闲块 | 可用“大窗口从高端分配/尺寸分档”缓解；彻底解决需要 MMU。先记录，不修 |
| 16.2 | **路径上限 64 字节（保留，取舍）** | `TOS_PATH_MAX`（`include/tos.h`）= 64；内核 syscall/cwd 和用户态工具共用 | 路径 ≥63 字符直接 -1，不截断。嵌入式上路径都很短 | 试过 128（`.bss` +544 B）和 256（+1696 B，另加每个 path syscall 的栈）；按“明显增体积不做”保留 64。真出现长路径需求再提 |
| 16.3 | ~~目录项名字 32 字节 + 点文件被吞~~ **已修（2026-11-23）** | `kernel/ramdisk.c`（`DENT_NAME 64`、只跳过 `.`/`..`） | 修前：`ls` 只显示 31 字符，点文件建得出来但列不出来。修后 C3 上 42 字符名字完整显示，`.hidden` 可建、可列、可读 | — |
| 16.4 | ~~全局打开文件数 8，但每进程 FD 12~~ **已修（2026-11-23）** | `kernel/ramdisk.c`（`MAX_OPEN 12`） | 修前系统级第 9 个 open 失败；修后与 `MAX_FDS` 一致。代价：`.bss` +1088 B（含 16.3 的 640 B），`.text` +80 B | — |
| 16.5 | **内核堆不可压缩** | `kernel/heap.c:6-12,104-140`（first-fit + 相邻合并） | 长期混合大小的分配（pipe 2 KiB、`argbuf = argc×256`、文件缓存）碎片化后，`kmalloc` 可能失败；只合并、不搬迁 | 当前规模可接受；压力测试里关注 `free` 的 peak/used |
| 16.6 | **栈只能翻倍增长，且与堆争窗口** | `kernel/process.c:3231`（`process_stack_grow`） | 堆先涨到 guard 附近后，深递归无法再扩栈；只能靠 `TOS_STACK()` 预留。会打印明确提示 | 保持；文档已说明 |
| 16.7 | **用户堆峰值不归还** | `user/heap.c:10-16` | `free` 合并后复用，但 brk 只增；一次性大分配永久抬高水位（空间仍可复用） | 设计如此；若需要可加“顶部整块回缩”，收益有限 |
| 16.8 | **控制台输出会丢字节** | `kernel/console.c:102`（trylock 失败整块丢）、`drivers/usb/rp2350/usb_cdc.c:603`（环满丢） | 长输出在 SMP 竞争或 USB 单核下不完整；#46 是后者 | #46 用 USB kthread 修；UART 路径的丢弃是刻意的 |
| 16.9 | 行输入 255 字符、raw 队列 512 B | `kernel/tty.h:23`、`kernel/tty.c:145` | 超长粘贴/命令行截断；`rx` 有流控所以不受影响 | 记录即可 |
| 16.10 | 不 wait 的子进程退出码最多记 8 个 | `kernel/process.c:42`（`LAG_MAX 8`） | 父进程连续 spawn 超过 8 个不 wait，最早的退出码丢失 | 记录；需要时按需调大 |
| 16.11 | `uptime` 32 位毫秒 | `board/*/platform.c` 的 `plat_uptime_ms`、`kernel/syscall.c:577` | 连续运行 49.7 天回绕；用它算的 `cpums` 会出错 | 若要长跑，改 64 位（syscall 返回仍 32 位的话另开 API） |
| 16.12 | 文件位置/大小 32 位 | `user/lib.h:169`（`lseek(int off)`）、`include/tos_abi.h:22`（`size uint32_t`） | 单文件 >2 GiB 不能 seek，>4 GiB 大小溢出；与 FAT32 上限一致 | 若将来上 FAT/大文件再改 ABI |
| 16.13 | littlefs 只增不缩 | `kernel/ramdisk.c` 的 `lfs_fs_grow` 路径 | 分区/镜像只能向大改；缩小要重刷 | 记录 |
| 16.14 | 固定容量：`PIPE_SIZE` 2 KiB、mailbox 4 条、`MAX_PROCS` 16/18 | `kernel/pipe.h:33`、`kernel/mailbox.c:18`、`kernel/process.c:35` | 满了会阻塞/返回错误，属于显式行为 | 不需要改 |
| 16.15 | ~~csc 的 native 表没有 `yield`/`poll`~~ **已加（2026-11-23）** | csc 表新增 `yield()` / `ready(fd)`；rvm 与 rcc 三个后端都实现 | `yield` 不再按 tick 量化；`ready` 是非阻塞 poll，事件循环可以只 step 就绪的 fd | 见 `files/tests/ready.c`；完整 `poll(fds,n,timeout)` 仍未暴露（csc 没有结构体指针） |
| 16.16 | **内核 flash 占用：board 层内联膨胀**（2026-11-25 立项） | `board/rp2350-generic/platform.c` 单 TU：`-O2` 78.6 KiB、只给 10 个冷打印函数加 `noinline` 后 19.1 KiB、`-Oz` 9.3 KiB；`-fsave-optimization-record` 显示 `diag_*`+`early_*` 被内联 171 次 | 该 TU 占 RP2350 内核镜像 47 %（`rpi-pico2-smp` 179.7 KiB、ATK SMP 185.9 KiB），其中约 64 KiB 是复制出来的冷代码 | 专项研究见 [#56](https://github.com/yafengabc/TinyOS/issues/56)：P1 冷路径 `noinline`（RAMFUNC 只 noinline 不加 cold）→ P2 扫其它 TU → P3 再谈逐文件 `-Os`；判据与测试矩阵写在 issue 里 |
| 2026-11-25 已落地一部分：`[target] opt` 旋钮（`TOS_OPT` > config > target > `-O2`），`esp32c3-generic` 默认 `-Oz`（板级 5 轮 A/B 无退化，app 100.7 → 81.6 KiB，C3 accept 42/42）。M33 的 `-Oz` 与 P1 仍未做 |

## 17. x86 镜像超出 300 KiB（2026-11-25，pre-existing）

`go run ./cmd/tos build all` 现在是 **17/18**：`x86` 在 `MKFS` 阶段报
`lfs.c:691: No more free space`（`image already at the 307200-byte partition`）。
在**未含本次 RP2040 改动**的 HEAD 上同样失败，所以不是这次引入的；`docs/CI.md` §8
写过这个失败模式（"精简该目标的用户程序或扩大分区"）。

怀疑与 2026-11-25 落地的控制台短写/`write_all` 改动（`1a5f3e3`，每个用户程序多几
十字节）叠加后刚好压过 300 KiB，但没做 bisect。处理选项：
① 把 x86 的 `ramdisk_sectors` 从 600 提到 800（+100 KiB，PC 上不心疼）；
② 或从 x86 的默认程序表里去掉 `coremark`/`pio` 之类的大件；
③ 或先只记录，等下次动 x86 时一起处理。

## 18. exp/thread-sched：rv32 QEMU 上 `threadbench` 挂起（2026-11-25，pre-existing）

在 `exp/thread-sched` 上用临时的 `kthreads = true` 配置构建 `rv32`，`threadbench`
在 QEMU 上没有任何输出、也不返回提示符（等 22 s）；同一镜像里 `hello`、`kthreadtest`
正常，`kthreadtest` 的 488 次兄弟切换/0 次保护重编程也通过。用 **2ec7d57（S1）**
的 worktree 跑同一配置，`threadbench` 同样挂起，所以不是 S3a 引入的。

- 影响：只有这个基准程序在 rv32 QEMU 上；ARM QEMU（`configs/arm-kthreads.conf`）上
  `threadbench` 正常，`kthreadtest` 在 rv32/arm 都正常。
- 没做的事：没有 bisect，也没在真机上复现；C3 kthreads 镜像需要板。
- 处理方向：先用 `-d int`/`gdb` 看是卡在 `sched_pick` 还是用户循环；如果只是 TCG 太慢，
  把 `ITERS` 调小或加一个 `threadbench small` 入口。

## 19. S4：USB 泵线程的 Pico 2 RV32 板测（2026-11-25，待跑）

代码在 `exp/thread-sched`（见 `docs/THREAD-MODEL.md` §2f）。镜像：

```sh
go run ./cmd/tos build -config configs/rpi-pico2-rv32-usbpump.conf
# build/tinyos-rpi-pico2-rv32-smp.uf2
```

默认镜像不启用 `usb_pump`，行为与今天一致（tick 返回用户态时 poll）。

1. 启动先看 UART0 与枚举（CDC 出现前 UART0 先说话，避免把"主机还没枚举"当成死机）：

   ```sh
   python scripts/rp2350/pump_repro.py --uf2 build/tinyos-rpi-pico2-rv32-smp.uf2 \
       --uart-port COM19 --watch 40 --log build/pump-repro.log
   ```

   期望：UART0 走完全部阶段，`[usb] pump thread pid N on cpu 0` 出现一次，
   CDC（2e8a:000a）出现并应答 shell。

2. CDC 上：
   - `cat /proc/ktask`：应有一行 `usbpump`，size 6144，used/hw 是真实高水位
     （S0 的测量点；这个数字决定以后要不要把池缩小到 4 KiB 或分池）。
   - `ps`：`usbpump` 在列表里，prio 0，`wink 0.0/0`（ktask 没有窗口）。
   - `cat readme.txt`、`ls /bin`：短写路径。
   - 一个 ~21 KiB 文件 `cat` 两路逐字节一致（§5c 的判据）。
   - `crash`：fault 只走 UART0，shell 继续（§2c/§5c）。
   - `smpbench`：两个 hart 都跑满；泵固定在 core 0，core 1 的吞吐不应被拖。
   - `heartbeat` / `uptime`：tick 仍 1000 Hz。
3. 回退：用默认配置重建（`go run ./cmd/tos build -target rpi-pico2-rv32-smp`），
   确认 `[usb] pump thread` 不出现、控制器仍由旧钩子轮询。

结果写 `docs/acceptance/`，注明板子、端口、镜像与 `cat /proc/ktask` 的 used/hw。
