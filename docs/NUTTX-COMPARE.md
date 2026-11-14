# NuttX 对照笔记

> 目的：NuttX 的 RP2350 / RP2040 / ESP32-C3 端口和 TinyOS 目标集几乎重合，它踩过的
> 坑、做过的取舍可以直接借。本文只记录**能落地的东西**，不是介绍 NuttX。
>
> 信息来自 NuttX 官方文档（`nuttx.apache.org/docs/latest/platforms/{arm,risc-v}/rp23xx*`）、
> PR `apache/nuttx#16424`（RP2350 RISC-V 端口，2025-08 合入）和
> `Documentation/guides/protected_build`。仓库内的具体路径以 master 为准（本文只写
> 目录级位置，避免行号漂移）。

## 1. 同一个问题，两套答案

| 问题 | NuttX | TinyOS |
| --- | --- | --- |
| RP2350 双架构 | `arch/arm/rp23xx` 与 `arch/risc-v/rp23xx-riscv` 两套独立端口；RISC-V 端口文档标 *experimental* | `board/rp2350` 与 `board/rp2350rv`，同样两套 |
| 无 MMU 的用户态隔离 | `CONFIG_BUILD_PROTECTED`：内核/用户两个 blob + syscall 桩，**只支持 Cortex-M**（M0+ 无 MPU，只能 flat） | 内核 + 每程序一个可变窗口；ARM 用 PMSAv8、Hazard3 用 NAPOT、C3 用 TOR、RP2040 用软件守卫 |
| 多核下写 flash | `sched_lock()` + `up_cpu_pause()`（IPI 停核） | `rp_flash_park_other()`：标志 + 代际 ack + RAM 自旋环（`board/rp2350/platform.c`） |
| 程序加载 | 编进镜像的 app；FLAT 下 ELF 模块直接链进 base | `/bin` 里的压缩 ELF，`exec` 时流式解压、PIE 重定位进窗口 |
| 内存 | 全局堆，或 kernel/user 两个堆（`CONFIG_MM_KERNEL_HEAP`） | 窗口内 image + heap + stack 共享，grain 分配，栈可增长 |
| flash 抽象 | MTD 设备 + `BIOC_XIPBASE` ioctl | `plat_block_read/write/erase` + littlefs 直接挂 |
| 调度 | 可配 tickless（RP2350 用 alarm/oneshot） | 固定 1 ms tick |
| 足迹 | 文档：最小 32K，典型 64K+，全功能可到 100K+ | RP2040 内核 ~46 KiB、RP2350 ~65 KiB |

两边对 **Hazard3 外部中断** 的处理是同一个机制：`mcause = 11` + XH3IRQ 的
使能/挂起数组。我们的是 `drivers/usb/rp2350rv/usb_cdc.c` 里置 `meiea` 第 14 位、
`board/rp2350rv/platform.c` 的 `plat_irq_foreign()` 读 `meipa`。**上真机前值得和
他们的 `rp23xx-riscv` 中断代码逐行对一遍**，这是目前唯一完全盲写、又没验证过的
路径。

## 2. 值得直接借的四样

### 2.1 Hazard3 中断代码

`arch/risc-v/rp23xx-riscv/`（PR #16424 的核心）。看三处：

- XH3IRQ 的数组 CSR 编号与位序（我们用的是 `meiea/meipa` 0xbe0/0xbe1，元素 0 覆盖
  0..31 号线）；
- 外部中断在 trap 里的分发顺序（和我们 `plat_irq_is_tick` / `plat_irq_foreign`
  的两级判断对比）；
- USBCTRL 的线号（我们用 14）与 `INTE/INTS` 的清除顺序。

PR 里还有一句 "Remove rp23xx chip hw spinlocks/testset"，说明 Hazard3 上那套 SIO
自旋锁不能用——这点我们早就绕开了（`plat_spin_*` 用 `amoswap.w.aq`）。

### 2.2 ARM MPU 的 region 划分（顺带纠正一个我写错的前提）

NuttX 按架构分成两个文件，做法完全不同：

- **`arch/arm/src/armv8-m/arm_mpu.c`**（M23/M33，即 RP2350）：region 是
  **base + limit** 两个寄存器，32 字节粒度，**没有 2 的幂要求**：

  ```c
  limit = (base + size - 1) & MPU_RLAR_LIMIT_MASK;
  rbase = base & MPU_RBAR_BASE_MASK;
  putreg32(region, MPU_RNR);
  putreg32(rbase | flags1, MPU_RBAR);
  putreg32(limit | flags2 | MPU_RLAR_ENABLE, MPU_RLAR);
  ```

  也就是说 `[base, base+size)` 任意长度都能一条 region 描述，和 C3 的 TOR 一个形状。
- **`arch/arm/src/armv7-m/arm_mpu.c`**（M3/M4/M7）：RASR 只有"大小字段 + SRD
  子区"，这里的 2 的幂才是硬约束，所以他们写了
  `mpu_log2regionceil()` 取最近的 2 的幂，再用 `mpu_subregion()` 把首尾的 1/8
  子块关掉来逼近任意区间（`g_ls_regionmask[9] = {0,1,3,...,0xff}`）。**分块/对齐的
  那套麻烦只在 v7-M 存在。**

  另外它的 region 管理是一套小 API：`mpu_allocregion()`（位图 + `ffs(~mask)`）、
  `mpu_modify_region(region, …)`（切换时复用）、`mpu_configure_region()`（分配一个）、
  `mpu_freeregion()`、`mpu_dump_region()`（调试打印）。我们目前是写死的 region 号，
  没有位图。

我们自己的 `arch/arm/platform.c:803-830` 已经在用 v8 的 RBAR/RLAR 形式
（`limit = base + size - ARM_MPU_MIN_SIZE`），**所以 RP2350 的 4 KiB 窗口根本不需要
分块**——只要改常量、并把 `plat_window_needs_stack_alignment()` 改成 0。之前
OPEN-ITEMS 31.3 里写的"8 条 region、要算着用"是错的，那条已经改正。

### 2.3 MTD + `BIOC_XIPBASE`

`include/nuttx/mtd/mtd.h`、`drivers/mtd/`、`include/nuttx/fs/ioctl.h`。把 flash 当
块设备、把"这个设备能不能就地执行、XIP 基址在哪"用 ioctl 上报，好处是 littlefs、
FAT、TF 卡可以接在同一个接口后面。

我们现在是 `plat_block_*` 三个函数 + `PLAT_LFS_LBA/BLOCKS` 常量，够用但耦合了
"哪块 flash 属于文件系统"。如果以后要接 TF 卡或 PSRAM，值得换成 MTD 式的分层。

### 2.4 tickless

NuttX RP2350 端口支持 `CONFIG_SCHED_TICKLESS`，用 TIMER 的 alarm/oneshot 而不是
1 ms 中断。我们的 `sleep`/超时全部按 tick 计（`mtime` 在 RISC-V 侧已经走
`mtimecmp`），要做低功耗时这是现成路线。

### 2.5 RISC-V 的 PMP（`arch/risc-v/src/common/riscv_pmp.c`）

NuttX 把 PMP 藏在和 ARM **同一个** `mpu_configure_region(base, size, flags)` 接口
后面：内核只谈"段和权限"，由 `arm_mpu.c` 或 `riscv_pmp.c` 决定怎么写寄存器。这正
是我在本项目里说的"段模型 + 后端"，区别是我们的 `plat_task_protect()` 签名是
TOR 形状的六个边界，而他们是 `(base, size, flags)`。

四个能直接用的点：

1. **NAPOT 编码**：`napot = (base >> 2) | ((size - 1) >> 3)`。我们写的是
   `(base >> 2) | ((size >> 3) - 1)`，对 2 的幂两者等价；他们的形式是规范写法，
   照抄更不容易看错。
2. **他们的校验 vs 我们的分块**。2023 年他们补了 NAPOT 合法性检查（PR #10451）：
   `base` 必须按 `size` 对齐、`size` 必须是 2 的幂，否则直接 `-EINVAL`，因为
   "size 变大时可供编码 base 的位数变少"。我们走的是相反的路线——`pmp_cover()`
   把任意 4 KiB 对齐的区间拆成若干 2 的幂块，所以从不拒绝调用者，代价是消耗条目
   （`rp2350rv` 的 15 条预算）。两条都成立：他们要求调用者把区间做成 POT，我们
   让内核替调用者拆。
3. **TOR 是易错点**。他们修过两轮：PR #10451（NAPOT 校验）、PR #10724
   "fix broken TOR checks"（−28/+3 行）。根源是 TOR 的 `pmpaddr(N-1)` 耦合，以及
   region 0 的 bottom 恒为 0 这个特例。我们在 C3 上的 TOR 布局（5 个边界、
   entry 1..4、entry 3 故意 OFF 当护栏）正好压在这条耦合上，所以
   `prot rodata/heap/stack/null` 这四条是最小回归集，不能省。
4. **不要写死 region 编号**。PR #11864：bootloader 可能留下带 lock 位的 PMP 条目，
   所以他们改成不假设编号，用位图 `ffs(~mask)` 分配。我们在 Hazard3 上写死
   entry 5（trap vector）和 0..14（块）是安全的——复位后我们没有别人的 PMP 遗产
   ——但如果以后有第二个 RV 平台从 bootloader 手里接管，先读 `pmpcfg*` 跳过 `L`
   位，再按位图分配。

另外他们讨论过一个 RV32 细节：pmpaddr 是 34 位物理地址，编码大于 4 GiB 的区域时
需要 64 位中间值。我们的窗口都在 520 KiB 里，不涉及。

### 2.6 驱动模型

NuttX 的驱动是**两层**：上半（upper half）在 `drivers/` 里，注册给 VFS 并实现
`struct file_operations`（open/close/read/write/seek/ioctl/poll）；下半（lower
half）在 `arch/<arch>/src/<chip>/` 里，只做寄存器级动作，通过一个 ops 表被上半调用。

以串口为例（`drivers/serial/serial.c` + `arch/arm/src/stm32/stm32_serial.c`）：

```c
/* 上半：g_serialops 里的 uart_open/read/write/ioctl/poll */
/* 下半：只实现这些 */
static const struct uart_ops_s g_uart_ops = {
  .setup, .shutdown, .attach, .detach, .ioctl,
  .receive, .rxint, .rxavailable,
  .send, .txint, .txready, .txempty,
};
```

上半负责环形缓冲、回显、termios、阻塞/唤醒、信号（Ctrl-C）、锁；下半只有一个
"现在能发吗 / 发一个字节 / 收一个字节 / 开关中断"。一个上半可以服务任意多个下半
（USART、LPUART、外挂 16550、USB CDC 都是同一个上半）。另外它有一个我们特别该
学的接口：`uart_connected()` —— 可插拔设备掉线时让 pending 传输以 `ENOTCONN`
返回，而不是把调用者挂死。

块设备（`struct block_operations` + `register_blockdriver`）平时不直接给用户用，
而是交给 `mount()`；MTD（`struct mtd_dev_s`：erase/bread/bwrite + `MTDIOC_GEOMETRY`
/`MTDIOC_BULKERASE`/`MTDIOC_ISBAD`）可以叠：`sector512` 改页大小、
`mtd_partitions` 切分区、`ftl` 把 MTD 变块设备。

| NuttX | 我们现在的对应物 | 差距 |
| --- | --- | --- |
| `struct file_operations` + `register_driver` | `kernel/hwdev.c` 的静态设备表 + `switch (dev->type)` | 加一类设备要改 syscall 分发 |
| `uart_ops_s` / `serial.c` 上下半 | `plat_console_putc/getchar` + `kernel/tty.c` 的环 | 板级驱动和缓冲/回显混在一起 |
| `uart_connected()` | 无 | USB CDC 掉线没有统一语义 |
| `block_operations` + `mount` | `plat_block_read/write/erase` + 直接 littlefs | 没有块设备层 |
| `mtd_dev_s` + `BIOC_XIPBASE` | 同上，XIP 基址是常量 | 接 TF/PSRAM 时要重写 |
| `ioctl`（`BIOC_*`、`MTDIOC_*`） | 每类设备一个 syscall（`SYS_GPIO`、`SYS_ADC`…） | 新功能必须加 syscall |

**值得借的四条，按性价比**

1. **控制台的上下半**（最高）。C3 那个 `usj_putc` 在 trap 里空转 40000 次的 bug，
   本质上就是"下半被塞进了上半的位置"。把每块板缩成
   `{tx_ready, putc, getc, irq_on, irq_off}` 五六个函数，环、回显、阻塞和
   `process_console_kick` 留在内核一份，结构上就不可能再犯。
2. **`connected/disconnected` 语义**（便宜）。USB CDC 拔线后让读写返回 `-ENOTCONN`
   而不是等一个不会来的中断；我们现在是靠超时和"没主机就丢"绕过去的。
3. **`SYS_IOCTL(fd, cmd, arg)` 一个通用口**（便宜）。保留现有强类型 syscall，
   但给设备私有命令留一个逃生口，省掉"每加一个功能就加一个 syscall"。
4. **MTD 分层**（等有 TF/PSRAM 再做）。`plat_block_*` 现在够用，接第二种存储时
   再引入；`BIOC_XIPBASE` 那种"这块设备能不能就地执行"的元数据值得一起抄。

**不要借的**：VFS。NuttX 的 inode/挂载表/路径解析/dup 是为了 POSIX 兼容，我们的
整个 devfs 只有 20 项、736 字节的表；引进来会立刻变成内核里最大的一块。

### 2.7 调度器与 SMP

| NuttX | 我们 | 评价 |
| --- | --- | --- |
| `spin_lock_irqsave(&lock, &flags)` / `spin_unlock_irqrestore` | `plat_irq_save()` + `plat_spin_acquire()` 两个独立原语 | **最该借的一个**：§23.3 记的那个自死锁（持锁后中断没关，tick 重入同一把锁）靠命名把纪律固定下来 |
| `up_cpu_pause()`（IPI 停核，用于临界区里重配 MPU） | `rp_flash_park_other()` 代际握手 | 同一种做法，他们用于 MPU/EXCESSIVE 场景，我们只用于 flash |
| `CONFIG_SCHED_HPWORK` / `LPWORK`：内核工作队列（HP 是驱动下半，默认优先级 224；LP 支持优先级继承） | `g_idle_poll` 一个函数指针钩子 | 现在只有一个延迟用户（USB 轮询），够用；出现第二个（延迟擦除、日志落盘）时再上工作队列，别加第二个钩子 |
| `nxmutex` + `CONFIG_PRIORITY_INHERITANCE` | 自旋锁 + 调度器；控制台锁没有继承 | 我们现在所有临界区都很短（一次写、一次块操作），优先级反转的窗口有界；**如果以后出现长持有锁**（例如整文件拷贝期间持有 fs 锁），就必须上继承或把锁拆细 |
| tickless（alarm/oneshot） | 固定 1 ms tick | 低功耗才需要，见 2.4 |

他们调度器本身没有神秘之处（FIFO/RR + 优先级 + SMP 每核 run queue），真正值得抄的是**并发原语的名字和纪律**：`spin_lock_irqsave` 一眼就说清"锁 + 关中断"是原子的一步。

### 2.8 内存管理

| NuttX | 我们 | 评价 |
| --- | --- | --- |
| 大/小内存模型：每次分配 8 B 或 4 B 头，`CONFIG_MM_SMALL` 在 32 位 MCU 上强制 16 位头 | `kernel/heap.c` 单 arena | 8 KiB 堆上省下的头开销是几百字节，**不值得**为它改分配器 |
| 多 heap：`kmm_initialize` / `umm_initialize` + `mm_addregion`（外挂 PSRAM 当第二个区域） | 单一内核堆 | 等有 PSRAM（Pico Plus 2 那类板子）再说 |
| `mallinfo`（arena/ordblks/fordblks/uordblks） | `free` 里的 total/used/peak | 我们的 already 够用 |
| **granule allocator**：固定 granule 大小 + 用户指定对齐，`gran_initialize(heap, size, log2gran, log2align)`，位图管理；原版每次最多 32 granule（后来去掉了） | 窗口分配是扫 `procs[]` 的 first-fit，对齐靠 `align_up` | **值得考虑**：我们的"按最大块对齐"正是一个 granule 分配器的参数；4 KiB granule 下 48 KiB 窗口 = 12 个 granule，32 的上限也够。但 MAX_PROCS=16 时扫描成本无所谓，**等窗口数上去或要支持不连续窗口时再换** |
| 临界区用 `spin_lock_irqsave`（`CONFIG_GRAN_INTR`） | 同上 | 同 2.7 |

**结论**：调度器借**原语纪律**，内存借**granule 分配器的形状**（不是代码），其余保持现状。真正的收益都在"少写一类 bug"，不在功能。

## 3. 不要照抄的三条

1. **栈大小**：PR 里写 "Double size for idle, irq and main stacks"，我们刚把 ARM 内核
   栈从 16K 降到 8K，并且实测过。两边的中断模型不同（他们 tickless、我们是 tick），
   数字没有可比性；
2. **USB 状态**：NuttX 的 RP2350 ARM 端口写着 CDC/ACM "known to corrupt data and stop
   responding"，RISC-V 端口标 Working。这说明这块生态里**没有现成的正确实现可抄**，
   我们踩过的坑只能自己收；
3. **足迹对比**：他们 32K 是最小裁剪（无文件系统、无 shell），我们的 46 KiB 里包含
   shell、littlefs、ELF 加载器。比总量意义不大，要比"打开同一组功能之后"的数字。

## 4. 一句话结论

NuttX 在**广度**上是我们几十倍（POSIX、VFS、MTD、网络、电源管理），但在
**"无 MMU 芯片上的用户态隔离 + 动态装载"** 这一点上，TinyOS 的窗口模型反而更靠前：
NuttX 在 M0+ 上只能 flat，而我们用软件守卫做出了可检测的失败。

所以借用方式应该是：**机制照抄（中断、MPU 划分、MTD），策略不跟**（栈大小、tick、
足迹）。
