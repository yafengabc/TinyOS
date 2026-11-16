# TinyOS 项目审查报告（2026-11-16）

- **基线**：`ccdbc92`（已把本地快进到 `origin/main` 最新；本地 `rv32emu-c` 子模块指针改动和两个未跟踪文件未受影响）。
- **范围**：本次聚焦上次 review 没覆盖的部分——SMP 并发、异常/启动、板级 flash 与 DMA；并复核 11-11 review 的 7 项是否落地（旧报告已删除，结论并入本报告第一节）。上次已排除的第三方代码、纯风格问题同样排除。
- **方式**：静态调用链审查 + 关键证据逐条亲自复核 + ARM 目标实测构建。未改任何实现代码。
- **说明**：高/中严重度结论均给出可定位的代码证据；除 ARM 构建外未做动态复现，也不声称已利用。

---

## 一、11-11 的 7 项落地情况：7 项里 6 项已修，1 项部分落地

| 编号 | 问题 | 现状 | 证据 |
|---|---|---|---|
| F-01 | `SYS_READDIR(len=0)` 内核写 `ptr[-1]` | **已修** | `kernel/syscall.c:394-397` 在复制前显式 `if (len == 0) return 0`，注释写明防止特权态写 `ptr[-1]` |
| F-02 | 任务窗口尺寸加法回绕 | **已修** | `kernel/process.c:1860-1884` 改用 `add_u32_ok()` / `align_up_ok()`，回绕即 `proc_retire` 返回 |
| F-03 | ELF 程序头表 `phoff + phnum*size` 回绕 | **已修** | `kernel/elf.c:187-196` 加 `ELF_MAX_PHDR` 上限并改用 `phoff > size \|\| phnum > (size-phoff)/sizeof(Phdr)` |
| F-04 | ELF note 三处长度回绕 | **已修** | `kernel/elf.c:282-297` 新增 `note_step()` 限界；`internal/build/build.go:303-338` 用 uint64 逐步检查（mklfs 同构已确认） |
| F-05 | 默认 ARM 目标构建失败 | **已修并实测** | 本次 `go run ./cmd/tos build -target arm` 成功，产物 `build/kernel-arm.elf`，`build-config.txt` 已生成 |
| F-06 | CI 未覆盖 QEMU 功能测试 | **部分落地** | `.github/workflows/images.yml:156-192` 新增 smoke job，覆盖 rv32/rv32smp/arm 的构建 + smoke + `soak.py` 三轮，并作为 release 前置（`:197 needs: [build, smoke]`）。仍缺：`go test ./...`、`go vet ./...`；x86 因 16 位重定位仍未纳入（OPEN-ITEMS §5） |
| F-07 | 非法配置值静默吞掉 | **已修** | `internal/build/config.go:231-277` 的 `truthy()`/`parseOnOff()`/`parseSize()` 对非法值返回 `error`，含溢出检查 |

上次报告最有价值的三项内核内存安全（F-01/02/03）都做了正确修复，不是表面打补丁。唯一遗留的 F-06 半项（单测/vet/x86）已在 `OPEN-ITEMS.md` §5 明确记账。

---

## 二、本次新发现

### N-01｜高｜RP2350 的 SysTick 优先级修复被初始化顺序悄悄回退，USB 抢占实际失效

**位置**：`kernel/main.c:49-50`、`board/rp2350/platform.c:776-777`、`arch/arm/platform.c:381`、`arch/arm/scs.h:41`、`drivers/usb/rp2350/tusb_shim.c:96`

**证据链**（已逐段核对）：
1. `kmain()` 先 `plat_early_init()` 再 `plat_init()`（`main.c:49-50`）；
2. `plat_early_init()` → `plat_board_early()` → `plat_console_init()`，在 `platform.c:776` 把 SysTick 优先级从 0x00 降到 0x20，注释明确目标是"让 USBCTRL(0x00) 能抢占 tick，保证 SET_ADDRESS 微秒预算"；
3. 紧接着 `plat_init()` → `arm_exceptions_init()` 在 `platform.c:381` 无条件写 `ARM_SHPR3 = (ARM_PRIO_SYSTICK<<24)|...`，而 `scs.h:41` 定义 `ARM_PRIO_SYSTICK = 0x00`；
4. → SysTick 被写回 0x00，与 `tusb_shim.c:96` 设为 0x00 的 USBCTRL 同优先级，Cortex-M 下同优先级异常互不抢占。

**后果**：板级那段有明确硬件理由的修复完全不生效。tick handler 里有诊断输出和 USB 轮询（板级注释自述"长到足以耗尽 SET_ADDRESS 微秒预算"），枚举延迟仍取决于 tick 恰好在做什么。SMP 第二核经 `plat_cpu_secondary_init()` 再次调用 `arm_exceptions_init()`，同样写回 0x00。

**修法**：让 SysTick 优先级由板级在 `arm_exceptions_init()` **内部**决定（例如 `board.h` 增加可覆盖的 `TOS_PRIO_SYSTICK`，rp2350 定义 0x20），删除 `platform.c:776-777` 这个时机错误的写入；或加 `plat_arm_exceptions_init()` 弱钩子在写 SHPR3 之后调用。

### N-02｜中高｜RP2350 RV32：flash park 超时错误路径永久关闭中断

**位置**：`board/rp2350rv/platform.c:1009,1012`（erase）、`:1061,1064`（write）、`:908,985-987`（`rp_flash_park_other`）

**证据**：调用者先 `csrrci was_enabled, mstatus, 8` 清中断，`rp_flash_park_other()` 内部**再次** `csrrci mstatus, 8`，此时 bit8 已是 0；超时分支 `if (mstatus & 8u) csrsi mstatus,8` 因此永远为假，超时返回 -1 后调用者又在 `:1012` 直接 `return -1`，没有恢复 `was_enabled`。成功路径正常（调用者末尾恢复）。

**触发**：SMP 下另一 hart 在 2000 万次循环内未给出 ack（另一核卡死或未调度）。

**后果**：`mstatus.MIE` 永久为 0，tick 和控制台中断全停，系统看似死锁，只能等看门狗。注意**只在超时错误路径**坏——正常擦写没问题，这解释了为什么常规验收通过而偶发楔死难查。

**修法**：`:1012`/`:1064` 返回前补 `if (was_enabled & 8u) csrsi mstatus,8;`；或像 ARM 版（`platform.c:1416-1421` / `:1392-1393`）那样把中断状态保存/恢复职责收敛到一处。

### N-03｜中高｜RP2350 ARM：`wdt_feed()` 未钉在 RAM，XIP 关闭窗口内的取指靠编译器内联运气

**位置**：`board/rp2350/platform.c:467-470`、`1425,1434`、`1468,1477`

**证据**：`wdt_feed()` 是普通 `static void`，无 section/noinline 属性。它在 `plat_block_erase/write()` 的 `g_rom_exit_xip()` 与 `flash_enable_xip_via_boot2()` 之间被调用（`:1434`），此时 QMI 处于 serial command mode、XIP 失效。当前所有构建里它被编译器内联进 RAMFUNC 调用者，属于巧合。**同一仓库的 RV32 端口已经修掉这个坑**：`rp2350rv/platform.c:721-733` 定义 `RAMFUNC_WDT = __attribute__((section(".data.ramfunc"), noinline))`，注释逐字说明"这正是靠内联运气会变成死板的缺陷"。ARM 端口未同步。

**后果**：改优化级别、开 LTO 或增加调用点导致 outline，就会在两核 park 且中断关闭时从 XIP 取指 → 总线错误/静默挂死，表现为写随机失败且无输出、看门狗复位。

**修法**：照搬 RV32 版的两行属性到 ARM 版 `wdt_feed()`/`wdt_enable()`。

### N-04｜中｜`process_signal()` 全程不持 `proc_lock`，SMP 上可致错杀/半初始化任务被选中

**位置**：`kernel/process.c:2476-2546`（`process_signal`）、`:406-417`（`proc_by_pid` 无锁扫描）、`:2502,2504,2523,2530-2540`（无锁字段写）

**证据**：`victim = proc_by_pid(pid)` 无锁；SIGSTOP/SIGCONT 直接写 `victim->stop_pending`、`victim->state`，而调度器 `make_ready`/`sched_pick`/`sched_switch`、回收 `retire_and_reap` 全部在 `proc_lock` 下访问同一批字段。`:2511` 的 `kprintf` 会在 `out_lock` 上等待，显著拉长取指针到用指针之间的窗口。

**触发**：`SYS_SIGNAL`（`kernel/syscall.c`，任意核）或 CPU0 tick 的 `^C/^Z`（`kernel/tty.c`）并发于另一核 retire/reuse 该槽位。

**后果**：写已 `ST_FREE` 或已换主的槽位；`SIGCONT` 把 `ST_NEW`（window/frame 尚未建立）的任务标成 `ST_READY` 提前运行 → HardFault/越界；`^Z` 被 `next->state = ST_RUNNING` 覆盖而静默丢失。

**修法**：用 `plat_spin_lock_irqsave(&proc_lock)` 包住 `proc_by_pid` + 状态判断 + 所有 pending/state 写；提供 `task_kill_locked()` 复用同一临界区，`task_release` 须确认 victim 仍存活；`preempt_if_higher` 放到解锁后。

### N-05｜中｜sleep 唤醒扫描的条件判断在 `proc_lock` 之外（双 tick 重复唤醒）

**位置**：`kernel/process.c:1005-1011`

**证据**：每核都有 tick，`p->state` 的读取在锁外，`make_ready(p,0)` 内部才加锁且**不复核 state** 就无条件写 `ST_READY` 和 frame 返回值。与本文件 `:852-857`、`:905-912` 自己写下的"检查与状态变更须同一临界区"原则相悖。

**后果**：A 核唤醒 → B 核 `sched_pick` 选中并置 `ST_RUNNING` → 陈旧的另一次 `make_ready` 把 `ST_READY` 写回正在运行的任务，返回值丢失，甚至两核持有同一任务。

**修法**：在 `process_tick` 中一次性持 `proc_lock`，锁内完成到期 sleeper 的 frame 写 + `state=ST_READY`，锁外统一 `preempt_if_higher`。

### N-06｜中｜LCD DMA `dma_armed` 置位晚于 DMA 启动，嵌套调用可撕_frame

**位置**：`drivers/display/st7789/rp2350/lcd_st7789.c:154-209`（RV32 版同构）

**证据**：`:205-208` 先写 `DMA_CH_CTRL_TRIG` 启动 DMA，之后才 `dma_armed = 1`；`lcd_dma_sync()` 开头 `if (!dma_armed) return;`，而 `:183` 的 `lcd_cs(1)` 与 `:187` 的 `dma_armed=0` 之间无锁/无关中断。文件注释自述存在"SVC 控制台输出嵌套在 tick 绘制中"的场景。

**后果**：嵌套的 `lcd_dma_sync()` 看到 `dma_armed==0` 立即返回，不等当前 DMA/CS 就在 SPI1 上发起第二笔传输；外层随后 `lcd_cs(1)` 在新传输中途拉高片选 → 花屏、SPI FIFO 状态错乱。

**修法**：`dma_armed=1` 移到写 `CTRL_TRIG` 之前并加 `dmb`；更稳妥是用覆盖整个 `lcd_data_phase → kick → sync` 的锁（rowbuf 也是共享的）。

### N-07｜低｜其他一致性问题

- `arch/arm/platform.c:705-708` 的 `sio_gate_irq_restore()` 写 `PRIMASK` 后缺 `isb`，与同文件 `plat_irq_restore()`（`:493-501` 明确带 `isb`）不一致。
- `arch/rv32/platform.c:329-339` 的 TOR PMP 数组在"3 段 2 洞"输入下 `k` 可自增两次越过上界写 `addr[5]`；当前唯一调用方最多产生 1 个洞，故不可触发。
- `kernel/cpu.c:156` 的 `cpu_count++` 非原子；当前 `PLAT_MAX_CPUS=2` 只有一个二级核写，不可触发。
- 第二核发布 `cpu_release` magic（`:119`）缺显式 release 屏障，对端 `start.S:180-202` 缺 acquire；boot 核写后的 2000 万次自旋提供充足 drain，实际风险低。

---

## 三、已核查为正确、不作为问题的区域

- **7c50a90 的 console kick 修复**：选帧 `p->is_ktask && p->kframe ? p->kframe : &p->saved` 与 `make_ready` 一致，且整段在 `proc_lock` 临界区内，未引入回归。
- **锁原语**：RV32 `amoswap.w.aqrl` + `fence`、ARM LDREX/STREX 与 SIO 硬件门、`plat_flash_park_poll` 进 park 前关中断，语义均正确。
- **每核状态**：ARM 的 SHPR/SHCSR/VTOR/MPU、RV32 的独立 `mtimecmp`、`kstack_guard` 逐核布防，冷启动路径完整。
- **双核 park 握手**：`g_park_req/g_park_ack` 逐代确认正确；4 KiB 擦除范围取整无多擦；`ramdisk.c` 块层无越界写。
- **异常帧**：ARM `regs_t` 与 `start.S` 的 `FR_*` 偏移一致且有静态断言，PSP/MSP 切换与 MPU 着陆区自洽（M33 `deep 6` 属已记录的设计取舍）。

---

## 四、建议修复顺序

1. **立即（有硬件时延/静默死板后果）**：N-01（SysTick 优先级时机）、N-02（RV32 park 超时关中断）、N-03（ARM `wdt_feed` RAM 驻留）。三条都有同仓库另一端口的现成正确实现，改动量各在 1–5 行。
2. **本轮（内核并发）**：N-04、N-05——把信号与唤醒扫描纳入 `proc_lock` 的单一临界区，并补 SMP 回归。
3. **本轮（驱动）**：N-06 LCD DMA 竞态；与 SPI1/TF 共享总线锁一并考虑。
4. **后续清理**：N-07 各项，以及上次遗留的 F-06 半项（`go test`/`go vet` 进 CI）。

---

# 附录：冗余与优化机会（同日追加）

这一节回答"哪里可以改成更健壮、更优化的代码"。分三类：**值得做的冗余消除**、**真实的性能/内存优化**、**刻意重复——不建议动**。所有结论基于逐段 diff 和行号核对；死代码与结构体布局已亲自复核。

## A. 冗余消除（按收益/风险排序）

### A-1｜高收益/低风险：`wdt_feed` 的 RAM 驻留分叉，就是 N-03 的根因

`board/rp2350/platform.c:467` 的 `wdt_feed()` 是普通函数；`board/rp2350rv/platform.c:726-733` 有 `RAMFUNC_WDT = __attribute__((section(".data.ramfunc"), noinline))` 且注释逐字说明"靠内联运气会变成死板"。两处 watchdog 注释前 9 行相同，寄存器宏相同。**修 N-03 时顺手删掉 `RAMFUNC_WDT`，统一用两侧都有的 `RAMFUNC` 宏**（`rp2350:1173` / `rp2350rv:698` 定义完全相同）。1–2 行。

### A-2｜高收益/低风险：boot2 copyout / XIP 恢复 15 行逐字相同

`board/rp2350/platform.c:1195-1209` 与 `board/rp2350rv/platform.c:809-823` diff 为空，连 `+1u` 是 Thumb bit 的注释措辞都一样；静态变量与宏（`BOOTRAM_BASE 0x400e0000`、`BOOT2_SIZE_WORDS 64`）也同构。提到 `board/rp2350_flash_xip.h` 作 `static RAMFUNC inline`，两板只留宏注入。纯搬运。

### A-3｜高收益/低-中风险：RP2350 ARM/RV32 外设驱动约 500 行近乎逐字重复

- `board/rp2350/board_devs.c` vs `rp2350rv`：153 行，**仅 2 处差异**（include 的 memmap 头、板名字符串）。
- `drivers/gpio/rp2350/gpio.c` vs `rp2350rv`：217 行，**仅 1 行 include 不同**；SPI 驱动 87 行同理；`spi.h`、`board_internal.h` 两版完全相同。

可提取单份驱动 + 编译期注入 memmap。**前提**是先把 `arm_memmap.h`（259 行）与 `rp2350rv_memmap.h`（232 行）拆成「公共寄存器基址 + 架构专属」两层——共有的 XOSC/CLOCKS/RESETS/UART0/PLL_SYS/SIO/IO_BANK0/PADS 完全一致，差异是 ARM 的 SCS/SysTick/SPI/ADC/DMA 与 RV32 的 `SIO_MTIME*`/`TICKS_BASE`/ROM lookup。**注意**：这些驱动在 `plat_board_early()` 阶段使用，早于 `heap_init`/`devfs_init`/`process_init`，所以只能走编译期头参数化或链接期弱符号，**不能**走 `g_idle_poll` 这类运行时回调。

### A-4｜高收益/中风险：flash park 协议三份实现约 430 行同构，且已两次分叉出 bug

`board/rp2040/platform.c:887-973`、`rp2350:1220-1403`、`rp2350rv:836-996` 是同一套四件套（park loop / poll / park_other / unpark），逐代 ack、`g_flash_park*` 语义、自旋次数、"先 mask 中断再发布 ack"都相同。差异在中断原语（`cpsid`/`primask` vs `csrrci mstatus`）、屏障（`dmb`/`sev` vs `fence`/`h3.unblock`）、诊断手段。**不要合并整个流程**——抽取协议骨架到共享头，用宏注入 `IRQ_SAVE/IRQ_RESTORE/BARRIER/WDT_FEED/PARK_DIAG`。这一条的价值主要是**防止再次分叉**：N-02 和 N-03 都出在这条路径上。

### A-5｜中收益/中风险：PL011 console 下层三板重复 ~40 行×3，且 getchar 已分叉

`rp2040:600-632`、`rp2350:824-863`、`rp2350rv:209-245` 的有界等待 putc/flush/tx_room 相同。更值得注意：`rp2350:864-906` 在 `plat_console_getchar()` 里直接调 `usb_cdc_poll()`，而 `rp2350rv:241-277` 把状态机移到 `g_idle_poll`，注释明确"tty_poll 在关中断/定时器中断内运行，TinyUSB 状态机不属于那里"——**RV32 是对的，ARM 应对齐**。改这条要复测 echo 与 USB SET_ADDRESS（见 N-01）。

### A-6｜中收益/中风险：时钟初始化 ~80 行重复，RV32 有必需的安全前置

`rp2350:185-276` 与 `rp2350rv:340-461` 的 XOSC 等待、`CLK_REF`/`CLK_PERI`、`PLL_SYS` 参数（REFDIV=1/FBDIV=125/PRIM=(5<<16)|(2<<12)）逐条对应。RV32 多的 `g_boot_*` 快照、`CLK_REF_SELECTED` 检查、切 clk_sys 离 PLL 的 5 行是硬件差异，**抽取时要留在板级调用序列**。测量函数统一到 ARM 版带 `src` 参数的 `measure_clk_src_khz()`。这段在 `plat_console_init()` 之前，属上电最敏感区，改动后必须实测 PLL 失败回退。

### A-7｜低优先级：conf/Targets 样板重复——**不建议现在做**

`exclude = wifiscan,wificonn,...` 在 10 个 conf 中重复；`build.go:208/224/233/242` 四个 RP2350 目标共享大部分字段。但每个 conf 头部注释都在解释"为什么这个目标排除这些程序"，抽成继承关系会降低可读性，改 `config.go` 解析逻辑的风险不值得。

## B. 性能与内存优化（按性价比排序）

### B-1｜最佳性价比：`tty_ctrl_deliver()` 无控制键时仍做 3 次全表扫描

`kernel/tty.c:594-602` 无条件调用 `process_ctrl_target()`（`process.c:1238-1260`，内部依次 `process_console_reader()` + `foreground_job()` + 自己再扫一遍 = 3×O(MAX_PROCS)），但 `pid` 只在 `ctrl_bs/ctrl_c/ctrl_z` 置位时用得到。把早退放到最前：`if (!ctrl_c && !ctrl_z && !ctrl_bs) { ctrl_age = 0; return; }`。默认 16 proc / 1000Hz 下每秒省约 48,000 次槽位读取，**零风险**。

### B-2｜易改：tick 每拍做一次除法 `1000u / plat_tick_hz()`

`kernel/process.c:978`。`plat_tick_hz()` 开机后不变（各板读全局），却在 1000Hz 每拍调用 + 32 位除法（RV32 无 M 扩展时是 `__udivsi3`）。在 `process_init()` 算一次 `static uint32_t ms_per_tick`，tick 用缓存值。风险极低。

### B-3｜易改：console 每输出一个字符调用一次 `process_current_pid()`

`kernel/console.c:69-82` 的 `out_char()` 每字符重读 `cpu_self()->cur`；一次 256 字节 `kprintf` 调 256 次。在 `console_write`/`kprintf`/`panic` 层取一次 `who`，传进批量输出函数。需保持 `out_owner`/`out_mid` 语义不变。

### B-4｜性能 + 正确性双收：sleep 唤醒扫描一次持锁

就是 N-05。当前每个到期任务各自 `make_ready()` 拿一次 irqsave 锁；改成扫描整体持 `proc_lock`，锁内写 frame/state，锁外统一 `preempt_if_higher()`。既省 N−1 次关中断+锁，又修掉双 tick 重复唤醒。

### B-5｜最大静态 RAM 节省：`file_t` 四个互斥指针改 union

`kernel/process.c:140-150` 的 `file_t`：`pipe`/`dev`/`devpriv`/`path` 四个指针共 16 字节，但一个 fd 只会是 CONSOLE/FILE/PIPE/CHRDEV 之一。改成 `union { struct pipe *pipe; struct { const dev_t *dev; void *devpriv; } chr; char *path; } u;`，结构从 36 → 24 字节。**`MAX_PROCS=16` × `fds[12]` 省 2,304 字节 .bss**（8 proc 省 1,152）。中等机械改动，约 10 个访问点（fd_clone/read/write/dup2/close），需逐个核对类型分支。

### B-6｜中收益：`bd_read()` 对齐路径绕开 4 KiB bounce + memcpy

`kernel/ramdisk.c:125-159` 所有读都经 `bounce[4096]` 再 memcpy。当 `(byte & 0x1FF)==0 && size 为 512 倍数` 时直接 `plat_block_read(..., buffer)`。exec 读 32 KiB = 64 次 512B 读，省约 32 KiB memcpy。需逐板确认 DMA 对目标缓冲的对齐要求。

### B-7｜中收益：`resolve_path()` 的 4 段缓冲接力

`kernel/syscall.c:61-131` 用 `cwd[64]` + `tmp[128]` + `norm[128]` + `seg[64]`，调用方还有 `upath[64]`+`path[64]`，单次路径 syscall 栈峰值约 512 字节。可直接用 `process_cwd_str()` 不拷贝、去掉 `tmp`、`seg` 在源串上原地判定。省约 192–256 字节栈。**归一化逻辑微妙，必须先把 `..` 回退和超长路径的测试补齐再改**。

### B-8｜小额可收缩点

- `procfs.c:25` 的 `PROC_CAP=1536` 按 `MAX_PROCS` 缩放（低配板省约 900 B 堆峰值）。
- `ramdisk.c:38` 的 `ofile_t.path[64]` 存了与 `file_t.path` 相同的路径；只存 4 字节哈希可省 480 B .bss，但哈希碰撞会误拒 unlink，需权衡。
- `heap.c:35-39` 的 `blk_t` header 16 B（`pad[8]`），理论上 8 B 即可维持 8 字节对齐，约省 200–300 B；要验证 littlefs 对齐假设。

### B-9｜**明确否决**的"经典优化"（简单胜过聪明）

- sleep 扫描改最小堆/定时器链表：16 槽×1000Hz 的线性扫描 <0.1% CPU，堆会加代码和 SMP 同步状态；B-4 已拿到主要收益。
- 空闲窗口分配 O(n²) 改区间树：exec 是一次性操作且本就持锁，代码自己已记录"16 任务=256 次比较"。
- `proc_by_pid`/`devfs_lookup` 改哈希：最多 16 proc / 32 dev，线性链更短且不在每拍路径。
- 块缓存替换策略 / 写合并：文件注释已实测"写合并缓冲从未承载一个字节"（prog_size==扇区），3 MiB 写入中 743 次擦除占约一半墙钟——**任何缓存策略都减少不了擦除次数**。别动 vendored littlefs。
- LZ4 逐字节解码改批量 memcpy：`lz4.c:131-133` 注释说明逐字节是为了跨 refill 边界与 overlap 语义。
- **`win_need` 与 `win_elf` 算两遍不是冗余**：`process.c:1860-1866` 与 `:1934-1952` 是防 hint 篡改的安全校验（F-01 的延伸），删掉等于去掉边界检查。

## C. 健壮性（让错误更容易被发现）

### C-1｜死代码：5 处，已逐个确认无调用点

| 位置 | 证据 | 处理 |
|---|---|---|
| `user/py/py_str.c:10,25` 的 `py_split`/`py_join` | 全树只有定义与声明；split/join 早已在 `py_rt.c:845,894` 用 `memcmp`+`sbuf` 重做 | 删这两个函数与 `py_str.c`，并从 `py_main.c:26`、`py_obj.c:4`、`py_rt.c:22`、`py_vm.c:26` 移除 include，再删只含这两个声明的 `py_str.h` |
| `internal/build/build.go:709-724` 的 `ldLldPath()` | 全树只有定义；`linkPIE()` 已走 `TC.UsesZig` 直接用 `zig cc` | 删 |
| `internal/build/ramdisk.go:213-216` 的 `ReadFileIfExists()` | 全树无调用 | 删 |
| `user/pio.c:211-218` 的 `map2()` | 全树只有定义 | 删 |
| `user/py/py_compile.c:438` | `static void compile_block(py_comp *cp);` 无定义无调用的悬空前置声明 | 删这一行 |

**不要误删**：`include/assert.h`、`include/ctype.h` 被 third_party 使用；`user/pio.c:969` 的第二个 `main()` 在 `#else` 无 PIO 板桩分支；`strspn/strcspn` 被 littlefs 使用。

### C-2｜`checkWindow()` 的加法未做回绕检查——校验会静默通过

`internal/build/build.go:366,370`：`need := w.span + w.stack + w.heap + uint32(guard)`、`rounded := (need + grain - 1)/grain*grain` 都是未检查的 uint32 加法，`w.heap`/`w.stack` 来自 ELF note，可做成接近 `UINT32_MAX`。内核侧同类问题已按 F-02 用 `add_u32_ok()` 修掉，构建工具侧没跟上。改用 uint64 并补一个 `heap_ask≈UINT32_MAX` 的 note 测试。

### C-3｜`WindowGeometry()` 的 16 KiB 时代数字——OPEN-ITEMS §4，仍未修

`build.go:260-266` 对四块板返回 `16K/8K/4K`，而四块 `layout.c` 全是 `4K/4K/4K`（`esp32c3:53,76,77`、`rp2040:29,34,35`、`rp2350:25,27,28`、`rp2350rv:23,25,26`）。导致 `checkWindow()` 拿错几何校验 `TOS_ONE_GRAIN()`，`msh.c:60` 的注解名不副实。两行改动，按 §4 单独提交。

### C-4｜ELF 头解析在 Go 侧有 3–4 份真相来源

`ramdisk.go:44-75`（十进制偏移）、`uf2.go:83-93`（同一批字段用十六进制重写）、`build.go:281-347`（`debug/elf`）、`lz4hint_test.go:54-65`（自带一套 descOff/next 算术）。抽 `readELF32Headers()` 用具名常量，并为畸形输入建一份共享语料——F-04 的建议里本来就要求"三个实现共享同一组畸形语料"，目前 C 侧统一了，Go 侧没有。

### C-5｜`go test`/`go vet` 仍不在 CI，且多数回归测试会静默跳过

`images.yml` 无基础检查 job；`internal/build/*_test.go` 有 24 处 `t.Skip`（如 `windownote_test.go:16,21,28`），没先跑 `tos build` 就跳过——"测了"不等于"覆盖了"。加基础 job，并把纯函数测试与依赖产物的测试分组，对 skip 数设下限断言。这就是 F-06 的遗留半项。

### C-6｜小规模健壮性改进

- `user/pio.c:220-293` 六个操作数解析函数都是 `strcmp` 链，且编号有洞写在代码里（跳 4/5、跳 3 等）；改 `{name,code}[]` 表 + `lookup()`，稀疏位置一眼可见。
- `user/py/py_rt.c:452-525` 四个比较运算符抄同一段 `memcmp` 四遍，且 `py_rt.c:472` 的 `/* Unary minus. */` 错位挂在 `py_op_le` 上；提 `py_cmp()` 并修注释。
- `py_code.h:159` 等七处的 `32` 没有一致性检查，`py_builtins[]` 有 13 项却无 `nbuiltin_names == py_builtins_count()` 断言；统一 `PY_BUILTIN_MAX` 并显式校验（`assert.h:16` 是 `((void)0)`，所以要么返回错误要么静态断言）。
- `user/softfloat32.c:405-475` 的 NaN 检查散落五处且策略不一致（`__ltsf2/__gtsf2` 没有前置检查，注释又说"每个 caller 都先检查"）；提 `either_nan()` 让策略显性化。
- `internal/build/toolchain.go:153-154,181-182,191-192` 三处逐字重复的 clang/lld 候选列表，提 `findClangAndLLD()`。

## D. **刻意重复，不建议动**（记录理由防止后人"好心办坏事"）

- **msh vs sh**：`msh.c:1-49` 注释明确 msh 是为塞进 16 KiB 窗口而砍特性（rp2040 只剩 60 B 余量），抽公共库会直接破坏体积预算。但已有语义漂移——`spawn()` 路径（`/bin/` vs `bin/`）、`sleep` 诊断文案、`var_set(NULL)` 行为都不同；建议只共享字符串宏或加"同一命令在两 shell 下输出一致"的 smoke，不合实现。
- **libc 两份**：`kernel/string.c` 是 word-at-a-time 优化版（littlefs 每次 block 拷贝三遍），`user/lib.c:61-67` 明确说用户程序不链 kernel 版所以补一份，`--gc-sections` 保证不占体积。但 `strchr` 语义不一致（kernel 版找不到结尾 NUL、user 版能），建议加共享一致性测试（`strchr(s,'\0')` 标准行为应返回结尾指针）。
- **SIO FIFO 唤醒**：ARM 用 `sev`/`wfe`，RV32 用 `slt x0,x0,x1`（Hazard3 的 `msleep` 不能被 fence 唤醒）；架构语义差异，合并会破坏正确性。
- **`lcd_delay_ms`**：ARM 轮询 `ARM_SYSTICK_VAL`，RV32 用 `TIMERAWL` 1 MHz 自由计数，是 31 行差异中唯一功能差异。
- **`layout.c` 的注释差异**：记录了"为什么这么设"（MPU vs PMP、boot ROM 行为、`df` 105% 事故），合并会丢失决策依据。
- **coremark 的 `ee_printf.c`**：基准测试自包含是设计。
- **rp2040 flash 路径**：用统一的 `rp_flash_op()` 合并 erase/write、无 watchdog、boot2 从 `XIP_BASE` 拷贝——是芯片差异不是风格分叉；只建议 rp2350 系列借鉴其"中断保存/恢复只在最外层一处"的结构。

## E. 追加建议顺序

1. **零风险清理**：C-1 的 5 处死代码 + B-1/B-2/B-3 三处十几行的热路径改写。
2. **顺手修 bug 的优化**：B-4（=N-05）、C-2（校验溢出）、C-3（OPEN-ITEMS §4 两行）。
3. **结构性收益**：B-5 的 `file_t` union（2.3 KiB .bss），需配回归测试；A-1/A-2 的 1–15 行低风险去重。
4. **成体系但需完整回归**：A-3/A-4/A-5/A-6 的跨端口抽象——价值主要在于**阻止"一个端口修了另一个没修"再次发生**，这条项目历史上已经付过学费。
5. **不要碰**：D 节全部条目。`go test`/`go vet` 进 CI（C-5）与 F-06 合并推进。
