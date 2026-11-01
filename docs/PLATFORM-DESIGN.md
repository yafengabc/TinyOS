# TinyOS 多平台架构设计（x86 / RV32 / ARM）

状态：草案 v2
目标读者：内核与移植开发者

## 1. 定位

TinyOS 面向 **无 MMU 的 MCU 级平台**：ESP32-C3（RV32IMC）、RP2040（Cortex-M0+）、
以及后续的 ARM 芯片。

设计原则：

- **有 MMU 的平台不折腾** —— 那类需求用 Linux 更合适。
- x86 端口仅作为**开发与仿真目标**，行为与 MCU 对齐：**不启用分页**。
- 统一模型是 **特权级隔离 + 单一平坦地址空间 + 无内存保护**。

这意味着一个明确的取舍：

> 用户程序不能执行特权指令，但**可以读写内核内存**。
> 一个越界写会破坏整个系统，这是 MCU 级 OS 的常态，不是缺陷。

## 2. 分层

```
user/            用户程序（架构无关 C，只依赖 syscall ABI）
────────────────────────────────────────────────────────
kernel/core/     架构无关：main、task、syscall、elf、ramdisk、heap、string、printf
kernel/include/  公共头：types.h、platform.h、tos.h
────────────────────────────────────────────────────────
arch/x86/        i386：GDT/IDT/TSS、PIT、UART、PS/2、ATA、VGA（不分页）
arch/rv32/       RV32：trap 入口、定时器、UART、块设备
arch/arm/        ARMv6-M/ARMv7-M：异常入口、SysTick、UART、块设备
board/pc/        板级：内存布局、启动
board/qemu-virt/ QEMU rv32 virt：0x80000000 启动、16550、CLINT
board/qemu-mps2/ QEMU Cortex-M 参考板
board/esp32c3/   ESP32-C3：ESP 镜像、SPI flash、UART0
board/rp2040/    RP2040：boot2、UF2、XIP、UART0
```

规则：

- `kernel/core` 不得出现 `inb/outb`、`cr3`、`mstatus`、`CONTROL` 等架构细节。
- `arch/*` 只能通过 `platform.h` 暴露能力。
- `board/*` 只提供地址、初始化顺序和驱动实例。

## 3. 平台接口（platform.h）

```c
/* ---- 启动 ---- */
void plat_early_init(void);
void plat_init(void);

/* ---- 控制台 ---- */
void plat_console_init(void);
void plat_console_putc(char c);
int  plat_console_getchar(void);     /* 无输入返回 -1 */

/* ---- 定时器 / 中断 ---- */
void     plat_timer_init(uint32_t hz);
uint64_t plat_ticks(void);
uint32_t plat_tick_hz(void);
void     plat_irq_enable(void);
void     plat_irq_disable(void);
void     plat_cpu_idle(void);        /* hlt / wfi */

/* ---- 块设备（littlefs 的底层） ---- */
int      plat_block_read(uint32_t lba, uint32_t count, void *buf);
int      plat_block_write(uint32_t lba, uint32_t count, const void *buf);
int      plat_block_erase(uint32_t lba);
uint32_t plat_block_size(void);
uint32_t plat_block_count(void);

/* ---- 物理内存（平坦模型：分配即可直接使用） ---- */
/* 没有帧分配器：RAM 直接可寻址，任务窗口由 kernel/process.c 切分 */

/* ---- 用户态进出 ---- */
void plat_set_kernel_stack(vaddr_t top);
void plat_enter_user(trapframe_t *tf) __attribute__((noreturn));
int  plat_fault_is_user(trapframe_t *tf);
void plat_task_protect(uint32_t win_base, uint32_t wx, uint32_t brk,
                       uint32_t stack_low, uint32_t stack_top);
void plat_panic_halt(void) __attribute__((noreturn));

/* ---- trap frame 视图（架构无关） ---- */
void     plat_frame_get_syscall(const trapframe_t *tf, syscall_t *sc);
void     plat_frame_set_ret(trapframe_t *tf, uint32_t val);
```

**没有 `plat_as_*`** —— 平坦地址空间下地址空间抽象是纯开销。
`trapframe_t` 由 `arch/*` 定义，core 只持有不透明结构；syscall 号与参数通过
`syscall_t {nr, a0, a1, a2}` 暴露，core 不直接读 `eax/ebx/ecx/edx`。
用户指针校验也不再是平台函数，而是 `process_user_ok(addr, len)`——只做窗口范围检查。

`plat_task_protect()` 是唯一带“内存保护”味道的接口，但语义仍然很薄：它只说
“现在要跑的这个任务，它的三段地址在哪里”，具体怎么表达交给端口——rv32 写成
PMP 条目，arm 写成 MPU 区域，x86 什么也不做（见 5.1、5.2）。堆与栈之间的空洞
大小是平台常量 `PLAT_HEAP_GUARD`（16 KiB），`process_sbrk()` 与能表达该空洞的
端口共用同一个数。

## 4. 平坦内存模型

所有地址就是物理地址，不存在虚拟/物理转换：

| 符号 | 含义 | 谁定义 |
|---|---|---|
| `KERNEL_BASE` | 内核加载地址 | 板级 |
| `RAM_BASE` / `RAM_SIZE` | 可管理内存 | 板级 |
| `APP_BASE` / `APP_MAX` | 用户程序窗口 | 板级 |
| `APP_STACK_TOP` | 用户栈顶 | 板级 |
| `APP_HEAP_BASE` | `sbrk` 起点 | 板级 |

各平台建议值：

| 平台 | RAM | APP_BASE | 说明 |
|---|---|---|---|
| x86 (QEMU) | 128 MiB @0 | `0x01000000` | 需 `-m 128M` 覆盖 |
| QEMU virt rv32 | 128 MiB @0x80000000 | `0x80400000` | |
| QEMU mps2-an385 (Cortex-M3) | 4 MiB @0x20000000 | `0x20080000` | 内核留 512 KiB；窗口 256 KiB × 14，`kernel.ld` 用 ASSERT 卡住不越界 |
| ESP32-C3 | 400 KiB @0x40380000 | `0x40390000` | 应用窗口 ≤ ~200 KiB；**必须用指令总线别名**，见 §8.1 |
| RP2040 | 264 KiB @0x20000000 | `0x20010000` | 应用窗口 ≤ ~120 KiB |

**关键后果：用户程序以 PIE（ET_DYN）+ 运行时重定位加载，跨板分发同一份二进制。**
加载器把镜像放到各板的 `APP_BASE`，重定位表（`.rel/.rela`）修正内部绝对地址，
同一份用户二进制可在 QEMU 与不同 `APP_BASE` 的真机间通用，无需按板重新链接。

## 5. 特权级

三个架构都提供“不能执行特权指令”，但内存保护的能力不同：

| | 用户态 | 陷入方式 | 内核栈 | 内存保护 |
|---|---|---|---|---|
| x86 | ring 3（分段平坦，**CR0.PG=0**） | `int 0x80` | TSS `esp0`（共用一层 16 KiB 内核栈） | 无（关分页后无从表达） |
| RV32 | U-mode | `ecall`（M 模式陷入） | 单一 32 KiB 内核栈，U 模式陷入时切换 | **PMP 分段**（P2.6 已实现） |
| ARM | Thread (PSP)，**非特权**（`CONTROL.nPRIV=1`） | `svc` | Handler 用 MSP，Thread 用 PSP；每任务 PSP | **MPU 区域**（P3 已实现） |

x86 关闭分页后：内核与用户共享 4 GiB 平坦空间，ring 3 仍能阻止 `cli/hlt/out` 等特权指令，
系统调用仍然走中断门。这与 C3/RP2040 的能力边界一致。

### 5.1 RV32 的 PMP 分段

RISC-V 对每次 U 模式访问都查 PMP，M 模式在没置 lock 位时不受约束——所以内核仍然
是那个平坦模型，只有用户侧被切开。`plat_task_protect()` 为**当前要跑的任务**写
4 个条目（`pmpcfg0`），切换时重写、`sbrk` 时也跟着更新：

| 条目 | 权限 | 范围 | 用途 |
|---|---|---|---|
| 0 | off | `win_base` | 只提供下界，本身不匹配任何地址 |
| 1 | RX | `[win_base, wx)` | 代码（`wx` = 第一个可写段的起点） |
| 2 | RW | `[wx, brk)` | 数据 + 堆，随 `sbrk` 生长 |
| 3 | RW | `[stack_low, stack_top)` | 栈，NAPOT 编码 |

两个要点：

- **TOR 是位置相关的**：条目 i 的下界是 `pmpaddr[i-1]<<2`，而且**上一条即使
  `A=OFF` 也照样提供下界**。条目 0 只能从地址 0 开始，所以“窗口以下全拒绝”
  就是靠这个空条目表达。实测确认（QEMU rv32 认这个语义）。
- **`brk` 与 `stack_low` 之间没有任何条目覆盖**，形成一段既不可读也不可写、
  更不可执行的空洞：堆写飞了、栈压穿了，都是当场 `store access fault`（`mtval`
  直接给出越界地址），内核只杀这一个任务。这比金丝雀强——金丝雀是事后发现，
  PMP 是当场拦下。金丝雀仍留在内核栈上，因为 M 模式不受 PMP 约束。
- 栈用 **NAPOT** 而不是 TOR：NAPOT 是绝对地址匹配，TOR 是位置相关——如果在栈那里
  用 TOR，下界就是 `brk`，中间那段空洞会被填上。NAPOT 要求“大小是 2 的幂、
  起始地址按大小对齐”，所以栈被放在窗口最顶端（`stack_top = base + WINDOW`，
  8 页 = 32 KiB，窗口是 1 MiB 对齐的）。

代价：每个任务 4 个条目（PMP 有 16 个），切换时 5 次 CSR 写。

### 5.2 ARM 的 MPU 区域

Cortex-M3 的 MPU 只有 8 个区域，而且每个区域是“自然对齐的 2 的幂”块，没有 TOR
那种任意上界。`plat_task_protect()` 用全部 8 个区域拼出一个任务：

| 区域 | 权限 | 范围 | 用途 |
|---|---|---|---|
| 0 | RW，**XN** | `[win_base, win_base + 256 KiB)` | 整个窗口：数据、堆、栈 |
| 1–6 | RO，可执行 | `[win_base, wx)`，从 32 B 起的 2 的幂块 | 代码 + 只读数据 |
| 7 | 特权 RW，用户不可访问 | `[stack_low - 16 KiB, stack_low)` | 栈溢出护栏 |

- **`PRIVDEFENA`**：`MPU_CTRL = ENABLE|PRIVDEFENA`。内核在 Handler 模式（以及
  第一个任务之前的 Thread/MSP）走默认内存映射，RAM/MMIO 全部可达；非特权访问
  默认映射一律拒绝。所以内核不需要给每个外设单写一个区域。
- **`nPRIV` 在 `arm_resume` 里、异常返回之前写**：Handler 模式忽略 `nPRIV`，
  所以写完之后、`bx lr` 之前到达的中断仍然以特权运行内核；只有异常返回装上的
  Thread 上下文是非特权的。只在 `EXC_RETURN & 4`（Thread/PSP）时置位——对
  Thread/MSP（第一个任务之前的 kmain）置位，第一次 tick 就会 MemManage。
- **RO 块从 32 字节起贪心**（ARMv7-M 最小区域 32 B）。`user.ld` 把可写段对齐到
  32 B，`[win_base, wx)` 因此能被精确覆盖；区域 0 置 XN，代码只能从 RO 块执行，
  数据/堆/栈不可执行——与 rv32 的 w^X 一致。
- **护栏 16 KiB** = `PLAT_HEAP_GUARD`，正好是 `process_sbrk()` 在堆和栈之间留下
  的空洞（`sbrk` 上限 = `stack_low - PLAT_HEAP_GUARD`），所以正常的堆写永远碰
  不到护栏。

与 rv32 PMP 的**已知差距**（都是“没有 TOR”造成的，如实记录）：

| 规则 | rv32 PMP | ARM MPU |
|---|---|---|
| 代码只读 | 精确到字节 | 32 B 粒度（`user.ld` 已对齐，实际精确） |
| 堆上界 | 精确到 `brk` | 只能到区域边界：`brk` 后写一字节不 fault，直到 `sbrk()` 覆盖它（`prot heap` 在 ARM 上会 “survived”） |
| 栈下空洞 | `[brk, stack_low)` 全部拒绝 | 只有栈下 16 KiB 护栏，再往下仍属于区域 0 |
| 栈溢出深度 | 任意深度都当场拦下 | ≤ 16 KiB 拦下，更深会踩进窗口 |
| 区域数量 | 16 个 PMP 条目 | 8 个（1 个窗口 + 6 个 RO 块 + 1 个护栏） |

实测（`run prot ...`）：`null`、`rodata`、`stack` 在 rv32 和 arm 上都当场
MemManage / access fault；`heap` 只有 rv32 能拦下。

## 6. 任务模型

```c
typedef struct task {
    int         state;        /* FREE / READY / RUNNING / WAIT_* */
    int         pid;
    int         slot;         /* 窗口槽位，-1 表示尚未分配 */
    int         wait_pid;     /* WAIT_CHILD：等哪个子任务 */
    int         exit_code;
    int         laggards;     /* 已退出但没被 wait 的子任务数 */
    vaddr_t     in_buf, in_len;  /* WAIT_INPUT：行读到哪个用户缓冲 */
    uint64_t    wake_tick;    /* WAIT_SLEEP */
    uint32_t    cpu_ticks;    /* ps 用 */
    vaddr_t     win_base;     /* 本任务窗口起点（无分页的下限保护） */
    vaddr_t     stack_top;    /* 窗口顶部留白后的栈顶 */
    vaddr_t     heap_base;    /* sbrk 起点：镜像之后 */
    vaddr_t     heap_end;     /* 栈底，sbrk 上限 */
    vaddr_t     brk;
    char        name[10];
    struct task *parent;
    trapframe_t frame;        /* 不在 CPU 上时的完整用户现场 */
    fd_t        fds[MAX_FDS];
} task_t;
```

- 没有地址空间字段：所有任务共享同一平坦内存。
- **每任务窗口**：从 `[APP_BASE, APP_MAX)` 按 `PLAT_APP_WINDOW` 切一块，
  镜像在底、sbrk 堆在中、栈在顶，用位图分配槽位。没有这一层，父子任务会互相
  覆盖对方的镜像——这是"平坦模型 + 多任务"必须补的一环。
- 用户态异常只杀当前任务，其余照跑。

### 6.1 调度：把 trap 收尾当成切换点

**只有一个内核栈**，但它天生是无状态的：从用户态来的 trap 一律先切到栈顶再压帧，
返回时把压进去的东西全部弹掉，两次 trap 之间栈上不留任何东西。于是任务的全部
状态就是 `task_t.frame`，而"切换"不过是让 trap 收尾去恢复**另一个任务的帧**：

```
trap_entry / isr_common          sched_switch(r)
  压用户现场 r  ───────────────►   current->frame = *r
                                   current->state = READY
  选下一个 READY                   next = 轮转（从 rr 游标往后找）
  ◄──────────────────────────────  return &next->frame
  按返回的帧恢复 → iret / mret       next->state = RUNNING
```

两个 arch 的收尾代码本来就接受"任意帧指针"：x86 的 `isr_common` 把
`isr_dispatch` 的返回值 `movl %eax,%esp` 后跳进共用的恢复段（与
`enter_user_frame` 同一段），rv32 的 `trap_handler` 返回 `regs_t *`，
`start.S` 里 `mv sp, a0` 后直接走 `.Lrestore`。所以切换不需要第二套汇编。

**为什么共用内核栈就够**：切换只发生在"trap 收尾、内核栈已空"的时刻。
trap 落进内核态（例如 syscall 执行中）时不会切换——`plat_fault_is_user()`
会拦下来，那次内核调用必须跑完，因为栈上压着它的调用帧，之后无法续跑。

### 6.2 阻塞：完成，而不是重放

没有可续跑的内核栈，就不可能"睡在 syscall 中间再醒过来接着跑"。这里的做法是
让 syscall **把请求记在任务里**，然后交出 CPU；谁来满足请求，谁就往任务的
`frame` 里写返回值，任务被恢复时已经在用户态拿到结果：

| 阻塞原因 | 记在任务里 | 谁来叫醒 | 恢复后 |
| --- | --- | --- | --- |
| `read(0)` 无整行 | `in_buf` / `in_len` | 定时器 tick 里 tty 行规程凑满一行 | `ret = 行长` |
| `wait(pid)` | `wait_pid` | 子任务 `exit` | `ret = 退出码` |
| `sleep(ms)` | `wake_tick` | tick 比较时钟 | `ret = 0` |

不需要"重启系统调用"（rewind `eip` 再执行一遍），所以不会有副作用重放问题。
所有阻塞态都是可抢占点。

### 6.3 抢占与抢占边界

- PIT / CLINT 每 10 ms 一次 tick；`process_tick()` 记 CPU 时间、唤醒睡够的、
  把凑满的输入行交给等待者，然后给当前任务扣一个配额。
- 配额 `QUANTUM = 5`（50 ms）用完置 `need_resched`，trap 收尾时切换。
- **只在用户态帧上抢占**：内核不抢占自己，理由见 6.1。
- 全部阻塞时进 `sched_idle()`：`sti/ei` + `hlt/wfi` 等中断，tick 负责唤醒。
- **内核栈守卫**：没有 MMU 就没有 guard page，栈写穿之后不会 fault，只会继续
  改写 `.bss`，最后从空洞里取指——一次和起因无关的野跳（串口驱动的 `'\b'`
  自递归就是这样崩在 `EIP=4b976946`）。现在栈底有一个 32 位哨兵，tick 里检查
  ① trap 帧在 `[__kernel_stack, __kernel_stack_top)` 内、② 哨兵未被改写，
  否则 `KERNEL PANIC: kernel stack overflow`。它不能阻止溢出，只能把结果从
  "随机野跳"变成一条可读的诊断，代价是每 tick 一次比较。
- **控制台是唯一的共享输出**：tick 里的回显和任务的 `write()` 抢同一个设备，
  而 tick 落在 `write()` 中间是常态（`cat` 分块写、后台程序同时在打印）。
  所以回显先进 `console_echo()` 的队列，只在"这一 tick 里没有任何任务输出"时
  才由 `console_echo_tick()` 放出；连续输出超过 50 tick（0.5 s）则强制放出，
  保证后台不停打印时输入也不会永远看不见。

### 6.4 语义与已知简化

- `exec` 是 **spawn**：返回子任务 pid，父任务继续跑；要结果就 `wait(pid)`。
  shell 的 `run` 就是 `exec` + `wait`，前台行为与以前一致；末尾加 `&` 变成后台。
- 没有僵尸：子任务退出立即释放窗口与表项。父任务正在 `wait` 就把退出码写进它的
  帧；否则记在父任务的 `exit_code`/`laggards` 里。**简化点**：并发多个子任务时
  `wait` 拿到的退出码不保证与 pid 对应（没有 per-child 状态队列），够用就好。
- 父任务先死时，孤儿子任务改挂到祖父任务（没有祖父就挂到 init）。
- init（第一个任务）退出即关机。
- 没有优先级、没有时间片捐赠、没有信号。

## 7. ELF 与重定位

- 保留标准 ELF32；`e_machine` 由 arch 校验（`EM_386=3`、`EM_RISCV=243`、`EM_ARM=40`）。
- 加载器工作变成“把 `PT_LOAD` 拷到 `p_vaddr`，清零 `.bss`”，不再有页映射。
- 链接地址 = `APP_BASE`，由板级链接脚本传入。
- 可选 LZ4TOS 信封：解压后仍是标准 ELF，三平台共用。
- 重定位：保留 PIE（ET_DYN）+ `.rel.dyn`（x86）/ `.rela.dyn`（RV32）运行时重定位。
  这是“同一份二进制跨板分发”的前提——已否决 MiniELF（TOSX）方案：
  它丢掉 ELF 结构与重定位表，二进制退化为板级绑定，不满足分发需求。
  现状：x86 加载器 `kernel/elf.c` 已实现 REL/RELA；RV32 侧复用同一路径。
- 段校验（`elf_probe`，加载前的唯一闸口）：段必须落在窗口内、不得回绕、
  `p_offset+p_filesz` 不得越出文件、**不得同时置 `PF_W|PF_X`**、`e_entry`
  必须落在某个可执行段内。两项新增检查的目的是让坏镜像在 `exec` 时就报错，
  而不是等它跑飞；注意窗口被切成 RX/RW 两带之后，w^X 在映射层面已经是结构性
  保证（一个段即使声明 RWX 也拿不到可写可执行的区间），这里是第二道闸。

## 8. 启动链

### x86（现状，需改造）

```
BIOS -> MBR(stage1.S) -> 内核 @0x10000 -> kmain
  改造：去掉 paging_init / CR3 切换；APP_BASE 移到 RAM 内
```

### QEMU virt（rv32，参考实现）

```
qemu-system-riscv32 -machine virt -bios none -kernel build/kernel-rv32.elf
  入口 0x80000000（M 模式），自带 trap 向量与设备初始化
  UART 16550 @0x10000000
  CLINT @0x02000000
  RAM 128 MiB @0x80000000
  块设备：内核内嵌的 littlefs 镜像（.data.ramdisk，RAM 盘）✅
  关机：SiFive test 设备 @0x00100000（0x5555 关机 / 0x7777 重启）
```

P2 落地的实现要点（`arch/rv32/`）：

- **trap 帧**（`regs.h`，144 字节）：x0–x31 按寄存器号顺序排列（偏移 = 4×寄存器号），
  后接 `mepc/mstatus/mcause/mtval`。帧里保存的 `sp` 是**被中断的栈**：U 模式陷入时
  内核先换到内核栈再压帧，`frame->sp` 就是 `mret` 要恢复的用户栈。
- **栈切换**：trap 入口读 `mstatus.MPP`；`MPP=U` 换到 `__kernel_stack_top`，`MPP=M`
  原地压栈。硬件在陷入时清 `mstatus.MIE`，所以入口序列不会被嵌套打断，可以拿
  `t0`/`mscratch` 当临时寄存器。内核里再触发的中断（如 `wfi` 期间的定时器）走
  "原地压栈" 分支，因此天然支持嵌套。
- **返回路径**按 `MPP` 决定恢复哪个 `sp`：U 模式回 `frame->sp`，M 模式回
  `frame + 144`。同一段代码被 `rv32_enter_user()` 复用，用来启动第一个用户程序。
- **`ecall`**：`a7` = 系统调用号，`a0/a1/a2` = 参数，返回写 `a0`。内核在派发**之前**
  把 `mepc += 4`，这样 `exec/exit` 保存的父任务现场和 i386 的 `iret` 语义一致
  （否则父任务会重放它的 `ecall`）。
- **PMP**：U 模式每个访问都要过 PMP，未配置则第一条用户指令就 fault。P2 先用一条
  NAPOT 全空间 RWX 条目（`pmpaddr0=0x3FFFFFFF`, `pmpcfg0=0x1F`）打通 U 模式，
  P2.6 换成了 5.1 的四条目分段。
- **内核栈**放在 `start.S` 的 `.bss` 里（32 KiB），`start.S` 清零 `[__bss_start,
  __bss_end)` 时一并清零，和 x86 的做法一致。
- **缺的软除法**：RV32 没有 64 位移位指令，clang 会为 `>>`/`<<` 生成
  `__lshrdi3/__ashldi3/__ashrdi3` 调用，`kernel/libgcc.c` 补齐了这三个（用两个
  32 位半字实现，避免递归调用自己）。

#### rv32 在 QEMU 下极慢：已解决（2026-11-01）

症状：banner 能打印，但列目录时约 4 秒才出一个文件（正常应为毫秒级），
`scripts/smoke.ps1 -Target rv32` 报 `shell prompt never appeared`。

**根因**：clang 的尺寸优化 `-Oz`（以及 `-Os`）生成的 RV32 代码在 QEMU TCG 下
每次循环迭代都被重新翻译（`-d in_asm` 显示同一循环体被翻译数十万次、每条指令
一个 TB），整个内核慢约 100 倍。`-O1`/`-O2` 生成的代码 TCG 可以正常缓存，
秒级启动。

**修复**：默认优化级别从 `-Oz` 改为 `-O2`（`build.go` 的 `ccFlags`）。
`-O2` 的 rv32 `.text` 为 31,654 字节（`-Oz` 为 27,040），只大 4.6 KB。
真机构建可用 `TOS_OPT=-Oz` 覆盖（真机上 `-Oz` 正常）。

排查过程记录（供参考）：排除过定时器中断风暴、UART 轮询、内存大小、
TB 缓存容量、单/多线程 TCG、CPU 型号、C 扩展、PMP 写入、guest 写代码页；
用带栈的裸机拷贝循环确认 QEMU TCG 本身对字节拷贝是快的（400 KB 约 1 ms）。

##### 同一个现象在 `-O2` 下会复发（2026-11-01 复核）

`-O2` 不是根治，只是让代码形状躲开了它。同一天的控制台/调度重构分支上
（同一个 QEMU 11.1.3、同样 `-O2`），rv32 启动又变成 8.96 秒，而 HEAD
（`559b71e`）只要 0.67 秒。定位到的**事实**（都在不加任何 `-d` 的情况下取得）：

| 观测 | HEAD | 复发时 |
|---|---|---|
| 启动到提示符 | 0.67 s | 8.96 s |
| `ls` 一次 | —（同量级） | 7.93 s |
| `plat_block_read` 累计 | 244 ms | 3 124 ms（同样 1697 次、848 KiB） |
| 启动期 memcpy 总量 | 30 069 次调用 / 1 824 KiB | 完全相同 |
| `info jit`：gen code / TB 数 | 113 598 TB / 39 MB | 2 344 943 TB / 794 MB |
| TB flush / invalidate 计数 | 0 / 0 | **0 / 0** |

也就是说：同样的字节数、同样的调用次数，拷贝本身慢了 13 倍；QEMU 一直在
重新翻译同一批 guest PC（5 秒内 230 万个 TB、794 MB 宿主机代码），却既没有
flush 也没有 invalidate。逐次迭代成本约 15–20 µs，与「每条指令重新翻译一次」
吻合；同一个 `memcpy` 循环在另一个二进制里只要 4 ns/迭代。

**两个观测陷阱**（都会把人带偏）：

1. `-d in_asm` 会让 QEMU 走「一条指令一个 TB」的日志模式（`info jit` 里
   `TB avg target size` 恒为 2 字节，快慢两版都一样），所以用它数「重译次数」
   数到的其实是执行次数。判据应该用 `info jit` 的 TB/flush/invalidate 计数
   加上墙钟，两者都不扰动 guest。
2. 现象**对布局/状态极其敏感**：只加一句 `perf` 预热（2000 次 512 字节拷贝）
   或仅仅把这几行代码放进二进制，同一个内核就从 9 秒变成 1 秒；一次
   `memcpy`、一次 `kmalloc/kfree`、20 ms 忙等都不能复现这个翻转。这解释了
   之前「把代码移出去就快了」和「尺寸敏感」的观察——它不是我们的 bug，
   但会把我们的启动时间随机放大一个数量级。

**落地对策**：与其和 TCG 的形状赌运气，不如把启动期的 I/O 量降下来。根因是
`readdir` 按索引实现、每个索引都从根目录重新走一遍，列 14 个文件要读 1697 个
块（848 KiB）、过 1.8 MB memcpy。改为**一趟走完填一张 20 项的目录快照**
（`kernel/ramdisk.c`，任何 create/write 都会让快照失效，因此不会读到旧名字）：

| | 改前 | 改后 |
|---|---|---|
| 启动到提示符 | 8.96 s | 1.73 s |
| `ls` | 7.93 s | 0.18 s |
| 目录块读取 | 1697 次 | ~120 次 |
| 代价 | — | x86 `.text` +480 B、`.bss` +720 B |

这条改动对真机同样有效（RP2040 12 MHz、C3 走 SPI flash，少读 1500 个块是
实打实的收益），并且让「TCG 抽风」时的损失从 9 秒降到 1.7 秒。

### QEMU mps2（ARM 参考）

```
qemu-system-arm -machine mps2-an385 -kernel build/kernel-arm.elf
  Cortex-M3，适合先验证异常/PSP/SysTick，再接 RP2040
```

#### 8.0 Cortex-M3 移植要点（P3）

硬件：SSRAM 4 MiB @`0x00000000`（代码，镜像加载地址就是 0，向量表头两个字号是
初始 MSP 与复位向量）、SRAM 4 MiB @`0x20000000`（`.data`/`.bss`/内核栈/用户窗口）、
CMSDK UART0 @`0x40004000`（DATA/STATE/CTRL/BAUD）、SysTick @`0xE000E010`（25 MHz
参考时钟，100 Hz 中断）。

异常模型：Thread 模式（PSP）跑任务，Handler 模式（MSP）跑内核，`svc #0` 进内核；
用户任务是非特权的（`CONTROL.nPRIV=1`），靠 §5.2 的 MPU 区域限制可达范围。
`nPRIV` 在 `arm_resume` 里、`bx lr` 之前写：Handler 模式忽略这一位，所以之后的
中断仍是特权，只有异常返回装上的 Thread 上下文非特权。不需要 PendSV——只有在
“把切换本身推迟到最低优先级异常”时才需要它。

踩过的五个坑，都有明确现象：

1. **帧布局不能有填充字。** `regs_t` 与 `arch/arm/start.S` 的 `FR_*` 偏移共享布局：
   20 个字号 = 80 字节，前 32 字节是 r4–r11，接着 `exc/vec/hw/kern_sp`，最后 8 个
   字号是硬件帧副本。多一个填充字会让 C 的 `pc` 读到 `xpsr`（`pc=0x0100000f`，
   `CFSR=0x40000` INVPC）。`arch/arm/regs.h` 里的 `arm_regs_frame_size_check`
   在编译期卡住大小。
2. **先 `sub sp,#48` 再 `push {r4-r11}`。** 反过来会把 r4–r11 推到帧的 +48（随后
   被硬件帧副本覆盖），`arm_resume` 从 +0 弹出的是内核栈里的陈旧内容：用户任务的
   callee-saved 寄存器全变垃圾，下一次 `sched_pick` 就用 `r6` 取 `procs[]` 而
   BusFault（`BFAR` 指向一个像指令字的地址）。
3. **`arm_resume` 必须按 EXC_RETURN 重设 PSP。** 切到另一个任务时（exec 出来的
   子任务），PSP 还停在上一个任务的用户栈上；只有 `0xFFFFFFFD`（Thread/PSP）
   的帧才需要在 `bx lr` 前 `msr psp, <帧保存的硬件帧地址>`。
4. **SysTick 必须比 SVCall 优先级高。** 任务在 syscall 里阻塞时是“SVCall active +
   `wfi`”等 tick；Cortex-M 里同优先级异常不能互相抢占，系统会永久停在
   `ICSR = 0x0400F80B`（VECTACTIVE=11，VECTPENDING=15）。`arch/arm/scs.h` 定义
   SysTick 0x00、SVCall 0x40、PendSV 0xE0，`plat_init()` 写进 SHPR2/SHPR3
   （偏移是 `0xD1C`/`0xD20`，千万别照 NVIC 的 `0x01C` 去算）。tick 打断内核调用
   不会重调度（`process_tick` 只看用户态帧），所以不存在内核栈被换走的竞态。
5. **MPU 的 SIZE 字段是 `log2(size) - 1`，不是 `log2(size)`。** 写错一次（256 KiB
   写成 128 KiB）的现象很误导：入口地址仍在区域 0 里，取指没问题，但异常返回时
   要从栈顶弹出硬件帧，而栈顶已经落在区域外，于是报 `MemManage ... at
   <入口地址>` 且 `MMFAR` 无效（出问题的是栈，不是 PC）。看到“入口就 fault、没有
   MMFAR”，先查区域覆盖到哪。另外 MemManage/BusFault/UsageFault 必须先在
   `SHCSR` 里使能（`plat_init()` 写 bit16–18），否则一律升级成 HardFault，`prot`
   就报不出具体异常名。

另外：`__aeabi_uldivmod` 必须按 ARM EABI 返回 `r0:r1` = 商、`r2:r3` = 余数。
用 C 的 16 字节结构体返回走的是隐藏指针，调用方读到的商是垃圾——表现为
`uptime` 永远是 0，而 `ps` 的 tick 计数正常。`kernel/libgcc.c` 里用汇编 shim
调 C 辅助函数修正，`__aeabi_uidivmod`/`__aeabi_idivmod` 同理（余数在 r1）。

QEMU 调试：`-S -gdb tcp::1234`，`gdb-multiarch build/kernel-arm.elf`（必须显式
传 ELF，否则断点不生效）。诊断帧问题看 `CFSR`@`0xE000ED28`、`HFSR`@`0xE000ED2C`、
`BFAR`@`0xE000ED38`、`ICSR`@`0xE000ED04`。

### ESP32-C3

```
ROM bootloader -> ESP 镜像（自定义头/段表）-> 内核
  UART0 @0x60000000
  SRAM 400 KiB，两个地址别名（见 §8.1）
  SPI flash 4 MiB：需要读/写/擦驱动
```

#### 8.1 SRAM 的两个别名：只能用指令总线那一个（真机实测）

C3 的同一块 SRAM 有两个地址：指令总线 `0x4038_0000`、数据总线 `0x3FC8_0000`，
相差 `0x700000`。QEMU 上两个别名都能取指，**真机上不能**，所以这不是可以
照抄模拟器的细节。用 `scripts/esp-c3/bus.sh` 写了三个探针在实板（C3 v0.3，4 MiB flash）上验证：

| 探针 | 布局 | 结果 | 含义 |
|---|---|---|---|
| `bus-code` | 全部 @`0x40380000` | 打印 `HI.` | 数据总线能读指令总线段 |
| `bus-data` | 代码 @`0x40380000`，数据 @`0x3FC80000` | 打印 `\xb7.` | 两个别名是同一块内存 |
| `bus-fetch` | 全部 @`0x3FC80000` | 无输出，`Saved PC` 落在 ROM | 指令**不能**从 `0x3FC8` 段取指 |

`bus-data` 打印的 `0xb7` 是 `0x40380000` 处第一条 `lui` 的 opcode 字节：ROM 先加载
数据段（写入 `0x3FC80000`），再加载代码段（写入 `0x40380000`），后者覆盖前者，
于是从数据别名读回来的是代码。这个"读到错的东西"恰好证明了两个别名同源。

结论：

- **代码、数据、堆、栈统一用 `0x4038_0000` 段编址。** 取指只认这一段，而 load/store
  两段都认，因此 §4 的单一平坦窗口模型在 C3 上原样成立，不需要把 ELF 的 `PF_X` 与
  `PF_W` 段重定位到两个地址空间。
- `RAM_BASE = 0x40380000`。用户可用 SRAM 上界是 `0x3FCDC710`（ROM 栈区结束处，
  见 docs/ZEPHYR-REFERENCE.md §2；此前误记为 `0x3FCCE710`），即 `0x403DC710`。
- PMP 条目里的地址同样用 `0x4038` 段，与 `sbrk`、`stack_top` 一致。

#### 8.2 两个观测陷阱（都会让人误判成硬件故障）

- **写进 UART FIFO 不等于发出去。** ROM 在跳到 entry 之前会打印最多 70 个字符，
  紧跟其后的写入排在这些文本后面；此时若程序复位，FIFO 被清空，字节无声消失。
  真机上表现就是"代码明明执行到了自旋处，却一个字符都没有"。探针里每次写入后
  都跟一段空转延时，输出才稳定。TinyOS 内核不受此影响——它在 `kmain` 里自己配置
  UART0，不依赖 ROM 留下的状态。
- **只用最后一次启动窗口判断会漏。** 串口抓包里每次复位都产生一个"entry 之后"
  的窗口，最后那个通常正好是空的。`scripts/esp-c3/capture.py` 因此报告所有窗口。

#### 8.3 PMP 交到手上时是全空的，16 个条目都没有锁

`scripts/esp-c3/probe.sh pmp` 读回 `pmpcfg0..3` 全为 `0x00000000`。但全零本身有歧义
——"条目关闭"和"条目不存在"读到的一样——所以 `umode.S` 往 `pmpaddr15` 写
`0xa5a5a5a5` 再读回，值保住了，说明 **16 个条目都是真的**。

- 没有被 ROM 预置的条目，也没有 `L=1` 的锁定条目，因此 M-mode 可以自由改写
  `pmpcfg`/`pmpaddr`，§5.1 的每任务四条目不撞墙。这是移植能继续的前提。
- 反过来说，**空 PMP 意味着 U-mode 的每一次访问都会被拒绝**：规范规定无条目匹配时
  M-mode 放行、U-mode 拒绝。所以"先配 PMP 再进 U-mode"不是防御性编程，是硬性顺序；
  忘了配，第一条取指就 fault。

#### 8.4 mtvec 是 WARL：256 字节对齐，且只支持 vectored

`scripts/esp-c3/probe.sh mtvec` 把四个值写进 `mtvec` 再读回：

| 写入 | 读回 |
|---|---|
| `0x12345678` | `0x12345601` |
| `0x403800c0` | `0x40380001` |
| `0x00000000` | `0x00000001` |
| `mepc <- 0x12345678` | `0x12345678` |

`mtvec` 的 bit[7:2] 恒为零、bit[1:0] 恒为 1（MODE = vectored）；同样要求四字节对齐的
`mepc` 却完整保留，所以这不是 CSR 读写的普遍问题，是 `mtvec` 自己的实现选择。

后果很实际：**把 trap 入口放在 `0x4038006c`，硬件读到的是 base `0x40380000`——也就是
`_start`**，于是任何一次 ecall 都把程序从头重启一遍，现象是输出里同一段横幅无限重复、
中间没有任何复位。`umode.S` 第一次跑就栽在这里，靠 `mtvec` 读回值才定位。

因此 C3 端口的硬约束：

- trap 入口按 **256 字节**对齐（`.balign 256`），不是规范说的 4 字节。
- vectored 模式下同步异常跳到 base 本身，中断跳到 `base + 4 × cause`；后者正好可以
  用来做 32 条中断跳转表，但每条只有 4 字节，只能放一条 `j`。

#### 8.5 U-mode 可用性（已验证）

`scripts/esp-c3/probe.sh umode` 一次验证四件事：给一条 1 GiB NAPOT（`0x4000_0000`
起、RWX）覆盖代码与 UART0，装好自己的向量，从 M-mode `mret` 进 U-mode，在 U-mode 里
写一个字符，再 `ecall` 回 M-mode。输出：

```
UM16            16 个 PMP 条目
acvM            pmpaddr0 / pmpcfg0 / mtvec 写入成功；M-mode ecall 自检返回
u               U-mode 写出的字符
T00000008       trap，mcause = 8（ecall from U-mode）
E403800bc       mepc
OK
```

M-mode、U-mode、PMP、mtvec、ecall 五条通路全部打通，C3 移植的可行性门槛清空。

两个踩过的坑记在这里，都是"看起来像硬件坏了"：`pmpaddr` 全 1 表示覆盖整个地址空间
是规范的边角条款，用显式 1 GiB NAPOT 编码更稳；`putc` 用 `t1` 存 UART 基址，会把调用
前读进 `t1` 的 CSR 值抹掉，探针里读回的 CSR 必须放 `s` 寄存器。

#### 8.6 tick 链路（已验证，2026-10-31）

TIMG0 → INTMTX 线 2 → vectored 槽 2，全链在实板上跑通，内核
`tick self-check` 通过、三个看门狗全关后 12 s 内零复位。板级序列在
`board/esp32c3/platform.c`，探针全过程在 `scripts/esp-c3/tick.S` 头注释。

- INTMTX：`MAP[32]=2`（源 32 = TG0_T0）、`PRI_2=1`（0x11C，不是 0x118）、
  `THRESH=1`、`ENABLE |= 1<<2`。**mcause = 0x80000000 | 线号**（tick 到达为
  0x80000002），不是规范 MEIP 的 11。
- 向量表：每槽**必须 4 字节**。不加 `.option norvc` 汇编器把 `j` 压成 2 字节
  `c.j`，`base + 4×cause` 落进表中间，中断被重新路由进槽 0 的同步异常处理器。
  同一修复必须应用到内核的 `trap_vectors`（qemu-virt 的 direct 模式掩盖此坑）。
- TIMG0：`REGCLK`(0xFC) 首先写 `0xE0000000`——bit31 默认 0（"寄存器不可读写"），
  bit30/29 默认 1；**单独写 bit31 会把两个默认 1 抹掉，反而关掉定时器功能时钟**。
  ALARMLO=10000、`CONFIG = EN|INCREASE|AUTORELOAD|ALARM_EN|79<<13`、INT_ENA=1。
  **寄存器读回是诚实的**（ALARMLO 读回 0x2710，CONFIG 逐位一致）。
- handler：每次 tick 都要 `INT_CLR` + **重写 CONFIG 的 ALARM_EN**（触发即自清零，
  AUTORELOAD 不恢复它，否则只来一拍）。完全静默的 handler（无任何 UART 访问）
  可以无限维持。
- handler **必须保存/恢复它碰过的所有寄存器**：100 Hz 的 tick 落进主循环
  `putc` 内部毁掉 t1/t2/t3 就能把主循环冻住。内核 `trap_entry` 全量保存帧，
  天然满足。
- C3 的 ROM 交接时 `mstatus.MIE = 1`（qemu 复位后是 0），`_start` 里显式
  `csrci mstatus, 8` 归一化。

看门狗（全在 `plat_board_init` 里关，写保护键同为 `0x50D83AA1`，super WDT 除外）：
TG0WDT（ROM 遗留，~1 s，`0x48` 整字写 0）、RTC WDT（esptool `--after no-reset`
遗留，RTC_CNTL `0x90`）、**super watchdog**（~3 s，`rst SUPER_WDT_RST`，SWD_CONF
0xAC bit30，键 `0x8F1D312A`）。esptool 烧录用的 `run.py` 走 `--after no-reset` +
手动 EN 脉冲，所以三个都得关。

##### 探针大坑：hex 打印循环吃掉待打印值

排查 tick 时绕了数小时的大弯路，根因是一个**打印 bug**：`put_hex8` 的循环里
`mv a0, t5`（数字字符）+ `slli a0, 4`，把待打印的值本身覆盖掉了——**只有第一个
nibble 是真的**。于是 entries < 0x10000000 时永远打出 `00000000`，被误读成
"零中断"；由此虚构出"handler 必须以 UART FIFO 写结尾"的假规律，连带把 SYSTIMER
备胎、UPDATE 踢、REGCLK 重写、GPIO 写等一整轮二分实验全部带偏。"TIMG 读回撒谎"
（ALARMLO/T0LO/CONFIG 恒 0）也是同一 bug 的伪证。教训：**探针的输出原语必须先于
实验被验证**——拿 `put_hex8(0x12345678)` 这种自证用例当第一课。

#### 8.7 SPI flash 块设备（已验证，2026-11-01）

littlefs 分区烧在 flash 偏移 0x100000（`PLAT_LFS_LBA=2048`，300 KiB=600 扇区），
`esptool write-flash 0x100000 build/ramdisk-esp32c3.bin`（脚本：`scripts/esp-c3/flash.py`）。
块设备三层组合，全部真机验证：

- **读 = ROM `esp_rom_spiflash_read`（0x40000130）**。ROM bootloader 已探测好芯片，
  零初始化即可用；dest 需 4 字节对齐。
- **写 = ROM `esp_rom_spiflash_write`（0x4000012c）**，外加最小变更比对：跳过与
  flash 现值相同的字（NOR 编程只能 1→0，重复编程同地址属非法）；需要 1→0 翻转
  说明漏了擦除，返回错误。
- **擦 = SPI1 控制器（SPI_MEM #1 @0x60002000）硬件命令位** `FLASH_WREN`（bit30）
  + `FLASH_SE`（bit24，地址在 ADDR_REG，4 KiB/扇区）。**ROM 的三个擦除入口全部
  不可用**：`esp_rom_spiflash_erase_sector` 与高层 `spi_flash_erase_sector` 返回
  0 但不擦（写过的 pattern 跨复位幸存），旧 API `SPI_sector_erase` 直接返回
  FAIL。硬件命令位是 R/W/SC：写 1 触发、完成自清，控制器自动轮询芯片 busy。

ROM 的芯片记录 boot 后是 **2 MiB**，所有 ROM flash 调用按它做范围检查
（`addr+len > chip_size` 即 FAIL，二分定位见 flashsz 探针）；boot 时必须调
`esp_rom_spiflash_config_param`（0x40000134）改成真实 4 MiB。实测芯片
JEDEC ID `0xC22016` = Macronix 4 MiB。

**双重偏移 bug 备忘**：`plat_block_*` 的 LBA 是**全局扇区号**（调用方传
`PLAT_LFS_LBA + first`），flash 地址 = `lba * 512`；板层若再加一次
`LFS_FLASH_OFF` 就会读到 0x200000 的陈旧数据——第一版实现正是这样把
旧 2 MiB 布局超级块当成分区内容，表现为 mount LFS_ERR_INVAL。定位手段：
对读回的 4 KiB 分 8 段做 checksum 与 host 镜像对比，7 段全对 1 段错。

**esp-riscv 核的 trap 取指不走 M-mode 权限**：U-mode 期间到达的中断跳
`mtvec + 4*cause`，**第一次取指按 U-mode 查 PMP**——向量表槽在 PMP 拒绝区时
表现为 mcause=1（instruction access fault）@ 槽地址。这与 RISC-V 规范相反
（规范：trap 首取指按新特权级；qemu-virt 即如此）。修复：boot 时用 PMP
entry 4 给 256 B 向量表开 U-mode RX（`pmp_open_trap_vector`，NAPOT）；
handler 本体仍在拒绝区，进入 M-mode 后执行。

真机端到端（C3 v0.3）：littlefs 挂载 14 文件、`wfile` 写 `/test.txt` →
复位后 `cat` 读回一致（真 NOR 持久化）、`ls/hello/uptime/echo/spin &`
（400 ms 抢占一跳）、`crash` 隔离、`prot rodata/null` 越界被 PMP 击杀。
`mem` 需 195 KiB 堆，超出 C3 的 64 KiB 窗口，malloc 失败是预期。

#### 8.8 任务布局：栈必须和它的尺寸对齐，否则 PMP 划错区间（真机发现，2026-11-01）

C3 上跑 `prot stack`（往栈底下面一字节写）**没有被杀**，而这四条越界用例在
qemu-virt 上全部 trap。原因不是 PMP 没生效，是**栈区 NAPOT 编码描述的不是我们
想要的那 32 KiB**：

- 窗口基址 `0x403AC000` 只有 16 KiB 对齐（内核结束在 `0x4039DA80`，窗口从它后面
  对齐到 16 KiB 才开始），于是 `stack_low = top - 32 KiB` 也只有 16 KiB 对齐；
- NAPOT 的编码 `(addr >> 2) | ((size >> 3) - 1)` 在基址未按 size 对齐时，硬件
  解码得到的是「**包含** stack_low 的那个 32 KiB 区」而不是「以 stack_low 开头的
  32 KiB 区」；
- 具体到 `[0x403C4000, 0x403CC000)`，实际生效的是 `[0x403C0000, 0x403C8000)`：
  守卫页（`0x403C0000..0x403C4000`）变成可写 —— 测试就是这么漏过去的 ——
  而真栈的上半截（`0x403C8000..0x403CC000`）反而不可用。

顺着这条线查出**同一类问题的第二个受害者**：RP2040 的窗口是 32 KiB，而栈大小是
全平台共用的 32 KiB、守卫 16 KiB，`span + 32 KiB + 16 KiB > 32 KiB` 恒成立，
`exec` 会永远回 `does not fit a 32 KiB task window`。它一直没被发现，是因为
Pico 还没有控制台，用户程序压根没跑起来过。

修法：**栈大小与守卫改为按板配置**（`PLAT_STACK_BYTES` / `PLAT_HEAP_GUARD` 放进
各自的 `layout.c`），并在 `process_init()` 里做一次四项自检（2 的幂、窗口是它的
整数倍、窗口容得下栈+守卫+镜像、`base + 窗口 - 栈` 按栈尺寸对齐），任一不成立
直接 panic 并说明是哪一项 —— 这类错误的表现（栈用一半 fault、守卫页可写、
exec 全被拒）跟布局八竿子打不着，靠症状反推太贵。

| 板 | 窗口 | 栈 | 守卫 | 镜像+堆可用 |
|---|---|---|---|---|
| pc (x86) | 4 MiB | 32 KiB | 16 KiB | 3.95 MiB |
| qemu-virt | 1 MiB | 32 KiB | 16 KiB | 976 KiB |
| qemu-mps2 | 256 KiB | 32 KiB | 16 KiB | 208 KiB |
| esp32c3 | 64 KiB | **16 KiB** | 16 KiB | 32 KiB |
| rp2040 | 32 KiB | **16 KiB** | **4 KiB** | 12 KiB |

真机复验：C3 `prot stack` 现在报 `store access fault at ... (address 0x403c3fff)`，
`scripts/esp-c3/accept.py` 18/18；x86/rv32/arm 的 smoke 与 compat 全绿。x86 代价
`.text` +224 B（自检与文案），`.bss` 不变。

**同一轮还修掉一个更难看的**：`build/ramdisk.bin` 是所有目标共用的名字，循环构建
时最后那个目标会覆盖它。于是「构建 rv32 → 构建 rp2040 → 烧 esp32c3」这条最常见的
顺序会把 **ARM 用户程序**烧进 C3，内核列出 14 个文件、然后拒绝每一个，报
"not a valid ELF32 program"，屏幕上没有任何线索指向文件系统不是它的。现在镜像按
目标命名（`build/ramdisk-<target>.bin`），并且 ELF 架构不匹配时会直接说
`program is machine 40 (arm), this kernel runs machine 243 (rv32)`。

### RP2040

```
ROM bootloader -> boot2（256 B，配置 XIP）-> 内核 @0x10000000（XIP）
  或：内核拷贝到 SRAM @0x20000000 运行
  UART0 @0x40034000
  SysTick（24 位）
  SRAM 264 KiB
  flash 2 MiB @0x10000000（XIP 只读，写入需 flash 驱动）
```

RP2040 需要一段 256 字节的 `boot2`（通常来自 pico-sdk），负责配置 QSPI XIP。
产物可用 UF2 格式，通过 ROM 的 USB 大容量设备拖拽烧录。

## 9. 行为一致性契约

| 项 | 约定 |
|---|---|
| 系统调用号 | `include/tos.h`，0–14 |
| 参数寄存器 | x86 `ebx/ecx/edx`；rv32 `a0/a1/a2`；arm `r0/r1/r2`，由 trap 入口统一映射 |
| 返回值 | 同上，负数表示错误 |
| 控制台 | `\n` 自动转 `\r\n`；退格回显 `\b \b` |
| 文件系统 | littlefs，块大小 4096，相同文件树 |
| ELF 错误文案 | 相同 |
| exec | spawn，返回子任务 pid；`wait(pid)` 取退出码 |
| 调度 | 时间片轮转，50 ms；只在用户态帧上抢占 |
| 内存保护 | x86 无（平坦）；rv32 PMP：code RX / data+heap RW / stack RW / 其余拒绝；arm MPU：窗口 RW+XN / code RO+可执行 / 栈下 16 KiB 护栏；内核读用户内存不受影响 |
| 异常隔离 | 只杀当前任务，其余继续跑 |

**`crash` 测试已经改写（P0 完成）。** 原先它靠写 `0xdeadb000` 触发缺页；关闭分页后没有缺页，
三平台行为无法一致。现在改为执行**非法指令**（x86 `ud2`、rv32 `.word 0`、arm `udf #0`），
三个架构上都产生“非法指令”异常，由内核终止当前任务。

验证方式：`scripts/compat.ps1` 对每个目标启动对应 QEMU，喂相同命令序列，
比较归一化输出（去掉时间戳和 pid）。

## 10. 迁移阶段

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P0** ✅ | x86 去分页 + 抽 `platform.h`；`crash` 改非法指令 | smoke 原样通过（除 crash 提示）——已完成 |
| **P1** ✅ | `arch/rv32` + `board/qemu-virt`：串口、定时器、trap | 打印 banner——已完成 |
| **P2** ✅ | RV32 ELF 直接加载 + ecall/U-mode + 内存盘 littlefs + 工具链探测 | smoke 全绿——已完成 |
| **P2.5** ✅ | 抢占式调度：任务状态机、时间片轮转、`exec`/`wait`、tty 行规程 | `ps` 里看到 `rdy`，后台 `spin` 与 shell 并行——已完成 |
| **P2.6** ✅ | RV32 PMP 分段 + 窗口重排（栈顶对齐、堆栈之间留洞）+ ELF w^X/入口校验 | `prot rodata/heap/stack/null` 四条都被拦下且只杀任务——已完成 |
| **P3** ✅ | `arch/arm` + `board/qemu-mps2`：异常、PSP、SysTick、用户态 ELF + littlefs | smoke 全绿（`hello`/`sh`/后台 `spin`/`crash` 只杀任务）——已完成 |
| **P3.1** ✅ | ARM MPU + `nPRIV=1`：每任务 8 个区域、RO 代码、栈护栏、`PLAT_HEAP_GUARD` | `prot null/rodata/stack` 当场 MemManage，smoke 全绿——已完成 |
| P4 | ESP32-C3：ESP 镜像 + SPI flash | 真机 `hello` |
| P5 | RP2040：boot2 + UF2 + SRAM 运行 | 真机 `hello` |
| P6 | 可选：PIE/重定位加固、优先级、每子任务退出码队列 | — |

P0 是最容易被低估的一步：**它会让 x86 失去现有的进程隔离**，
`crash` 的行为也会改变。这是刻意的取舍，不是回归。

## 11. 风险清单

| 风险 | 影响 | 缓解 |
|---|---|---|
| x86 无内存保护 | 用户程序可破坏内核 | 文档明确；`process_user_ok` 只做窗口范围检查；rv32 PMP / arm MPU 均已补上 |
| ARM MPU 无 TOR（只能 2 的幂对齐块） | 堆上界不精确、栈下空洞只有 16 KiB | 见 5.2 的差距表；`prot heap` 在 ARM 上预期不 fault |
| 用户二进制跨板分发 | 依赖加载器重定位正确性 | ELF32 ET_DYN + `.rel/.rela`，elf.c 校验目标窗口与 w^X |
| x86 关闭分页后 APP_BASE 需在 RAM 内 | 现有 `0x08048000` 超界 | 改链接地址，QEMU 内存覆盖 |
| C3 启动镜像格式 | 无法启动 | 先用 QEMU virt；或 ESP-IDF 二级引导 |
| C3 SPI flash 驱动 | 文件系统不可用 | 先 RAM 盘，后替换块设备 |
| RP2040 boot2/XIP | 无法启动 | 引入 pico-sdk 的 boot2 二进制 |
| Cortex-M0+ 无硬件除法 | 代码变大 | `-Os`；必要时软件除法 |
| ARMv6-M 不支持非对齐访问 | 结构体/协议解析崩溃 | `packed` 慎用；按字节读取 |
| 定时器差异 | `uptime/sleep` 不一致 | core 用 `plat_ticks()` 换算毫秒 |

## 12. 目录迁移草案

```
kernel/
  main.c task.c syscall.c elf.c ramdisk.c heap.c string.c
  console.c            # 纯格式化，不含 VGA/端口
  include/ platform.h types.h ...
arch/
  x86/  boot/stage1.S entry.S isr.S gdt.c idt.c pmm.c
        timer.c serial.c keyboard.c ata.c vga.c
  rv32/ start.S trap.S pmm.c timer.c uart.c block_ram.c
  arm/  start.S platform.c regs.h board.h scs.h usys.h
board/
  pc/        layout.h
  qemu-virt/ layout.h init.c
  qemu-mps2/ layout.h init.c
  esp32c3/   layout.h init.c flash.c image.h
  rp2040/    layout.h init.c boot2.h
```
