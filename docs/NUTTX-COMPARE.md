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
