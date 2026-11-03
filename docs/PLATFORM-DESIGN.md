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

/* ---- 电源 ---- */
void plat_shutdown(void) __attribute__((noreturn));
void plat_reboot(void) __attribute__((noreturn));   /* 先排空控制台 */

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
- **每任务窗口**：从 `[APP_BASE, APP_MAX)` 划一块，镜像在底、sbrk 堆在中、
  栈在顶。窗口大小不是固定的：`exec` 用 ELF 的 `memsz` 算出 span，加上栈和
  守卫洞，再按 `PLAT_APP_GRAIN`（C3 16 KiB）向上取整，向分配器要这么多。
  分配器直接扫描进程表里的活窗口找第一段够大的空隙——活窗口本身就是空闲表，
  不需要位图，也不会在 112 MiB 用户区上为位图大小发愁；任务一死，它的空隙
  自然与邻居合并。同一个 192 KiB 在 C3 上可以是 6 个 32 KiB 小任务，也可以是
  一个 160 KiB 的大程序。没有这一层，父子任务会互相覆盖对方的镜像——这是
  "平坦模型 + 多任务"必须补的一环。
- 用户态异常只杀当前任务，其余照跑。

### 6.1 调度：把 trap 收尾当成切换点

**只有一个内核栈**，但它天生是无状态的：从用户态来的 trap 一律先切到栈顶再压帧，
返回时把压进去的东西全部弹掉，两次 trap 之间栈上不留任何东西。于是任务的全部
状态就是 `task_t.frame`，而"切换"不过是让 trap 收尾去恢复**另一个任务的帧**：

```
trap_entry / isr_common          sched_switch(r)
  压用户现场 r  ───────────────►   current->frame = *r
                                   current->state = READY
  选下一个 READY                   next = sched_pick()：三级从高到低，
  ◄──────────────────────────────    级内从 rr 游标往后轮转
  按返回的帧恢复 → iret / mret       next->state = RUNNING
```

**三级优先级，0 最高**。任务出生在父任务的级别上；`setprio(pid, n)`（pid 0 = 自己）
只能把任务调到**不高于调用者自己**的级别——一个程序不能把自己抬得比 shell 还重要，
没有 aging 的世界里那一行就是锁死控制台的配方。级与级之间是严格的：只要高一级有
一个 READY，低一级就一个 tick 都拿不到；级内才是时间片轮转。低优先级饿死是**有意
行为**（shell 活着就该比后台拷贝先跑）。子任务出生时还没有跑过第一条指令，`nice`
在 `exec` 之后、`wait` 之前设级，所以不存在"用错误级别偷跑一片"的窗口。

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
- **唤醒也能抢占**：`make_ready()` 发现被唤醒任务的级别高于当前任务时直接置
  `need_resched`，切换同样等到 trap 收尾才发生。典型场景：一个 0 级任务在 tty 上
  睡着，一行输入到达的当拍它就拿回 CPU，而不是等 2 级的后台任务把这一片用完。
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
- 没有时间片捐赠、没有 aging（低优先级可能饿死，见 6.1）、没有信号。

### 6.5 命令文件：一个解析器，两个输入源

`source <file>` 与 `run sh <file>` 都在 `user/sh.c`，共用同一个 `split()` 和
`run_command()`，区别只在输入源和要不要 `exec`：

- `source` 把文件当成**当前进程**的输入源，递归调 `run_script()`。不占任务窗口。
- `run sh <file>` 就是 `exec("bin/sh", {"sh", file})`：新任务、新 pid、独立窗口，
  崩溃不牵连父 shell，行尾 `&` 可用，退出码经 `wait()` 拿得到。

为什么两条都要：C3 只有 **3 个任务窗口**（shell / 前台 / 备用）。`run sh a.sh`
自己吃掉一个，a.sh 里再 `run <prog>` 就是第三个，再嵌一层只会得到
`exec: out of task windows`——这是真机实测才暴露的，QEMU 上窗口宽裕，看不出来。
所以 `source` 是 C3 上唯一能嵌套的形式，代价是脚本跑飞会带走整个 shell（无隔离）。

三个细节都是 bug 换来的：

- `src_line()` 必须区分 **EOF(-1)** 与 **空行(0)**。最初两者都返回 0，于是每个脚本
  都在第一个空行结束（`etc/rc` 第 7 行）。
- 读缓冲属于源而不是 shell。`source` 把第二个文件套进第一个里，共用一个缓冲会冲掉
  外层"已读未用"的字节。`line[]` 反而可以共用——命令行在被完整解析前不会嵌套。
- 嵌套上限 `MAX_SOURCE = 4`，每层约 110 B（`src_t` 的 96 B 读缓冲 + 局部量），
  16 KiB 栈付得起；设上限的真正理由不是栈，是"两个脚本互相 source"这种死循环。

`exit` 在脚本里表示"结束本脚本并返回 N"（`& 255`，只有低 8 位），不关机；只有
`script_depth == 0`（控制台）时才是 `exit(0)`。启动脚本 `files/etc/rc` 由 pid 1
读一次（`getpid() == 1`，否则每层 `run sh` 都会重跑一遍），默认全注释：开机自己
说话的机器没法做差分测试，而这里打印的每个字节都会落在所有验收输出前面。

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

### 7.1 用户程序的体积构成：单文件多入口 vs 共享库（实测，2026-11-02）

14 个程序、rv32imc、`-Oz`、`-fPIC -pie`，命令行与 `tos build -target rv32` 完全一致，
产物总和与真实构建逐字节相同（20,756 B），所以下面的数字可以直接对照 `build/user/bin`。
（注：测于脚本系统合并之前，当时 sh 是 3,688 B；合并脚本与优先级后 sh 为 4,264 B、
14 程序总和 21,332 B。表内是同一批源码的内部对比，比例结论不受影响；真要实现多入口
时用当前源码重测一遍即可。）

| 形态 | 原始 | LZ4TOS 后 | 相对独立 ELF |
| --- | --- | --- | --- |
| 14 个独立 ELF | 20,756 | **14,572** | — |
| 14 个独立 ELF，去掉段头表 | 17,116 | 12,761 | −1,811（−12.4%） |
| 单文件多入口（1 个 ELF，14 个 applet） | 8,776 | **6,548** | −8,024（−55.1%） |
| 单文件多入口，去掉段头表 | 8,416 | 6,353 | −8,219（−56.4%） |
| 共享库（4,400 B 的 `libtos.so` + 14 个动态链接程序） | 27,012 | 17,152 | **+2,580（+17.7%）** |

跨架构结论一致（同样的 14 个程序、同样 `-Oz`，压缩后）：x86 −53.9%、rv32 −55.1%、
arm −57.7%。这个比例是**容器格式**决定的，不是 ISA 决定的。

**钱花在哪。** 一个空程序（`int main(void){return 0;}`）716 B，其中 `.text` 只有 22 B：
ELF 头 + 6 个程序头 244 B、`.dynamic` 64 B、段头表 + `.shstrtab` 240 B、
`.riscv.attributes` 54 B，其余是段对齐的填充。也就是说**每个程序约 690 B 是包装**。
再看真正逐字节重复的代码：跨 14 个程序只有 2,946 B（`putint` 740、`printf` 626、
`outbuf` 512、`puts` 264、`strlen` 220……）。所以单文件省下的 8,024 B 里，能归因到
"共享代码"的至多 2,946 B，剩下 5,078 B 只能是**包装**（容器、每文件的对齐与重定位表）。
这也是为什么"共享库"（只解决那 2,946 B）在这个规模上不划算。

**共享库为什么是负收益。** 三条独立的原因，任何一条都足以否掉：

1. 调用侧变贵。`jal` 4 B 变成走 GOT 的 `lui+ld+jalr` 12 B，再加每符号 PLT 桩 16 B、
   GOT 4 B、`.rela.dyn` 12 B；14 个程序合计 +1,856 B（`cat` +364，`sh` +236，
   连 `hello` 都 +204）。
2. 库本身 4,400 B（`.text` 1,334、`PLT` 96、`.dynamic/.dynsym/.dynstr/.hash/.gnu.hash`
   1,275、`.bss` 520），比它替代的 2,946 B 重复代码还大——`--gc-sections` 删不掉
   导出的函数，库一旦有 14 个用户就得整份留着。
3. 架构债。无 MMU 意味着**只有落在固定地址的代码能共享**，于是：要给每个任务加
   第 5、6 条 PMP（TOR 链必须递增，还得补一条"拒绝"区间）；加载器不能再对落在库
   地址区间的指针加 bias（要加一个范围判断）；库的 `.bss` 变成跨任务共享——
   `lib.c` 里 `printf` 的 512 B `outbuf` 和 `heap_ptr` 就是全局状态，
   两个任务同时打印会互相踩。现在每个任务各有自己的一份，这个 bug 出不来。

用数字写清条件：SO 划算当且仅当 `N·u > L + N·g`（`u` = 每个程序实际用到的库字节、
`g` = 每程序的调用侧开销、`L` = 库、`N` = 程序数）。这里是 `14×210 = 2,946` 对
`4,400 + 14×130 = 6,256`，差一倍还落在错的那边：库不算小，程序却平均只用掉 210 B 的它。
只有当每个程序真的用掉 ~450 B 库代码（今天 210 B）时左边才会翻过来——一份带浮点格式化
的肥 libc 是这种场景。现在这份 1.3 KB 的"libc"怎么做都是亏。同理否决"把 `printf`
下沉成内核 syscall"：变参表过不了系统调用边界，`printf(fmt, ...)` 的实参在用户栈上，
内核只能再发明一套参数编组。

**单文件多入口的代价**（都可接受，但要写下来）：

- 每次 `exec` 解压整块（6.5 KB → 8.8 KB），而不是几百字节的 applet。
- 内核堆峰值 = 压缩 6,548 + 解压 8,776 = 15.3 KB，占 32 KiB arena 的一半多一点
  （C3 曾经配到 64 KiB，2026-11-03 已收回，见下方"内核堆"一节）。真在意就把
  busybox 不压缩存（8,776 B，+2.2 KB flash），峰值降回 8.8 KB。
- rp2040 的镜像预算 = 窗口 32 KiB − 栈 16 KiB − 护栏 4 KiB = 12 KiB，8.1 KB 的 busybox
  只剩 3.9 KiB 余量（约 8 个 applet，再多就 `exec: does not fit a 32 KiB task window`）。
- vendored littlefs 是裁剪版，**没有 symlink**（`LFS_TYPE_SOFTLINK` 不存在），做不了
  符号链接农场。替代做法是查表回退：`process_create()` 里 `bin/<name>` 找不到就试
  `bin/busybox`，`argv[0]` 原样带过去，dispatch 按 `basename(argv[0])` 选 applet。
- 已验证能跑：14 个 `main` 改名成 `applet_*_main` 链成一个 8,776 B 的文件放进 FS，
  rv32 QEMU 上 `busybox hello alpha beta` / `busybox ls` / `busybox uptime` /
  `busybox hello_printf a b` 全通，`argv[0]` 与 argc 都对（`SMOKE TEST OK`）。
  dispatch 表 + 二分查找 ≈ 440 B。

**顺手可得的零架构改动**：段头表占 240 B/文件（`sh` 是 320 B），内核只读程序头
（`e_shoff` 在 `Elf32_Ehdr` 里只是定义，`kernel/elf.c` 从不读），链接后把
`e_shoff/e_shnum/e_shstrndx` 清零并截断文件即可，14 个程序省 3,640 B 原始 / 1,811 B
压缩（−12.4%）。它和"要不要单文件"是正交的两件事，也能单独做。

### 7.2 复测与三个新发现（2026-11-02，当前树）

脚本系统与优先级合并后 sh 从 3,688 长到 4,264 B，用当前源码重测（同 `tos build` 命令行，
rv32imc `-Oz`），并补了三件上一节没做的实验。

| 形态 | 原始 | LZ4TOS 后 | 相对独立 ELF |
| --- | --- | --- | --- |
| 14 个独立 ELF | 21,332 | 14,994 | — |
| 14 个独立 ELF，去掉段头表 | 17,692 | 13,180 | −12.1% |
| 单文件多入口（14 applet） | 9,352 | 6,953 | −53.6% |
| **分组打包（core 6 个 + tools 7 个）** | 7,648 | **5,879** | **−60.8%** |
| 共享库（4,436 B 库 + 14 个动态程序） | 27,672 | 17,610 | +17.5% |

**分组打包优于全塞一个**：组内 applet 共享度高，压缩率反而更高（76% vs 74%）；
全塞一个时不同 applet 的重复字节互相掩盖，LZ4 找不到更长的匹配。已真跑通（rv32 QEMU）：
`run core ls` / `run core echo hi` / `run core cat readme.txt` / `run core uptime` /
`run core mem` / `run tools hello alpha beta` / `run tools hello_printf x y` 全部正确，
`argv[0]` 被改写成 applet 名。

**发现一：共享库在当前内核上根本跑不起来。** 上一节只测了体积，没试过能不能跑。
实测把 SO 版 `hello` 放进 FS 执行 → `[pid 2] instruction access fault at 0x000000c0`。
根因：SO 用 `R_RISCV_JUMP_SLOT` 配 `.dynsym` 做绑定，而 `kernel/elf.c` **不解析动态符号表**
—— 它把 JUMP_SLOT 当成「本地函数」（S=0），算出 `addend + bias` 写进 GOT。SO 的
JUMP_SLOT addend 是 0，于是 GOT 里塞的是加载基址，跳过去是垃圾。`0xc0` 正是 `.plt`
段起始 vaddr（PLT0 解析桩）。所以 SO 的结论不只是「体积亏 17.5%」，而是
**要先给内核写一个动态链接器**，还要处理无 MMU 下的固定地址共享和库 `.bss` 跨任务共享。

**发现二：`exec` 的解压缓冲**不**泄漏**（上一版此处写的是"从不释放"，是错的）。
`kernel/process.c` 第 582 行把 `data` 重新指向解压出的 `raw`，第 630 行的
`ramdisk_release(data)` 于是释放的是 `raw`（`ramdisk_release()` 就是 `kfree()`）。
失败路径同样配平：第 579 行 `kfree(raw)`，第 571/589/598/605/612/625 行的
`ramdisk_release(data)` 各对应一条退出分支。

实测（临时加 `heap_used_bytes()` 探针，在解压后、释放前、释放后、proc_create 末尾
四个点各打一次，测完回滚）：

```
HP decompress 'bin/sh':   raw=4456 used=4456     <- 解压后，raw 还活着
HP before release 'bin/sh': used=4456
HP after  release 'bin/sh': used=0               <- 释放后归零
HP proc_create done 'bin/sh': used=0 peak=7664
```

独立程序（sh/ls/hello/uptime/ls）与分组包（sh/core/tools/core/tools）各跑一遍，
**每次 exec 结束后 used 都回到 0**，堆峰值只由最大的那次解压决定（7,664 B / 8,464 B）。

上一版的错误来自探针只打了一个点（解压后、`raw` 尚在），把**瞬时占用**当成了
**累计占用**：4,456 → 2,888 → 4,760 读起来像在累加，其实每次都是 0 起步、涨到本次
的 raw 大小、再归零。教训：量泄漏必须看"事件前后"的差值，单点快照证明不了任何事。

**发现三：多入口的 exec 峰值内存可以靠分组压下来。** 每次 `exec` 都要解压整块，
哪怕只想跑 `ls`。分组把这个代价从「全部 14 个」降到「6–7 个」。

**综合结论**：共享库否决；多入口值得做，推荐**分组打包**而不是全塞一个。
分组打包让每个 applet 的 exec 峰值从 9,352 B 降到 2,888–4,760 B。

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
`tostest smoke -target rv32` 报 `shell prompt never appeared`。

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

##### 这个坑在用户程序里一样存在，RV32EMU 没有（2026-11-02 实测）

上面两次都是内核自己的 `memcpy`。为了看清它到底是「QEMU 慢」还是「形状敏感」，
`user/bench.c` 用**同一份用户二进制**跑了两个循环（`tostest bench` 从宿主机
给 START/END 打时间戳；guest 的 `uptime()` 在 QEMU 是墙钟、在 RV32EMU 是指令计数，
不能当秒表用）：

| 循环 | 每趟迭代 | RV32EMU | QEMU rv32 | 比值 |
|---|---|---|---|---|
| 逐字节 `lbu/sb` | 5 条指令 / 1 B | 39.5 ns | **12 278 ns** | 311× |
| 逐字 `c.lw/c.sw` | 4 条指令 / 4 B | 37.4 ns | **12 075 ns** | 323× |
| 逐字节再来一遍 | 同上 | 38.6 ns | **12 537 ns** | 325× |

载荷各 256 KiB（4 KiB × 64 趟），QEMU 全程 7.5 s、RV32EMU 10 ms。三点结论：

1. **QEMU 的代价是「每次迭代」，不是「每个字节」**：1 B/迭代和 4 B/迭代都是
   12 µs，也就是 5 条指令和 4 条指令一样贵。这就是 TB 每次迭代被重译的指纹
   （≈12 µs 与之前测到的 15–20 µs 同量级，那是内核版）。
2. **第二次跑一模一样快**，所以不是冷启动/预热，是稳态行为——`-d in_asm` 那套
   取证方法的坑（见上）在这里不适用，这次两个时钟都独立同意了。
3. **RV32EMU 没有这个问题**：a1 与 a2 完全一致，逐字节/逐字的比值等于指令数比，
   代价随**执行的指令数**走。它是纯解释器（`RunHot` 每次都从内存重新解码），
   没有代码缓存，也就没有「缓存失效」这种失败模式可继承。
   顺带量到：用户态（U-mode，访存走 `Step()` 冷路径）约 **8–9 ns/指令**，
   内核态（M-mode，`RunHot`）约 **3.6 ns/指令**——U-mode 慢 2 倍多，因为每条
   访存都要过 PMP 检查并整条退回 `Step()`。

对写代码的指导不变、但理由更硬了：**热路径按字/多字节拷贝**。逐字循环在 QEMU 上
便宜 4 倍，正是因为它把迭代数降到 1/4；在 RV32EMU 上则几乎无感（35 vs 39 ns）。


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

littlefs 分区烧在 flash 偏移 0x80000（`PLAT_LFS_LBA=1024`，前 512 KiB 留给内核；3.5 MiB = 896 块），
`esptool write-flash 0x80000 build/ramdisk-esp32c3.bin`（脚本：`scripts/esp-c3/flash.py`）。
块设备三层组合，全部真机验证：

- **读 = ROM `esp_rom_spiflash_read`（0x40000130）**。ROM bootloader 已探测好芯片，
  零初始化即可用；dest 需 4 字节对齐。
- **写 = ROM `esp_rom_spiflash_write`（0x4000012c）**，直写。第一版带"最小变更
  比对"（先读回扇区、只编程与现值不同的字；NOR 只能 1→0，重复编程同地址属非法），
  正确但慢一倍：每个扇区多一次 512 B 读 + 一遍逐字比较，实测 44 KiB/s（2026-11-03）。
  唯一调用者 littlefs 只往刚擦过的块里写，每个要写的位本来就是 1，没什么可比的；
  擦除本身的正确性由 `spi1_command` 的 R/W/SC 位保证（命令位在芯片 busy 期间不清零，
  所以它返回时擦除真的完成了）。去掉比对后读次数减半、墙钟 +13%。
- **擦 = SPI1 控制器（SPI_MEM #1 @0x60002000）硬件命令位** `FLASH_WREN`（bit30）
  + `FLASH_SE`（bit24，地址在 ADDR_REG，4 KiB/扇区）。**ROM 的三个擦除入口全部
  不可用**：`esp_rom_spiflash_erase_sector` 与高层 `spi_flash_erase_sector` 返回
  0 但不擦（写过的 pattern 跨复位幸存），旧 API `SPI_sector_erase` 直接返回
  FAIL。硬件命令位是 R/W/SC：写 1 触发、完成自清，控制器自动轮询芯片 busy。

**小镜像 + 首启扩容（2026-11-03）**：烧录的 littlefs 镜像只有 300 KiB（600 扇区，
与其他目标同规格），分区是 3.5 MiB（896 块）。`ramdisk_init` 挂载时
`cfg.block_count = 0`——littlefs 从镜像超级块取尺寸、不与设备比对（lfs.c 的
"block_count=0" 分支）——挂载成功后 `lfs_fs_grow(PLAT_LFS_BLOCKS)` 把超级块
改成 896。分配上这依赖 **v2.9 的树可达性判空闲**：`lfs_alloc_scan` 遍历整棵
文件树标记被引用块，其余一律视为空闲，**不看擦除态**；块在首次分配时才擦
（`lfs_ctz_extend` 拿到块后紧跟 `lfs_bd_erase`）。所以旧 3.5 MiB 镜像在尾部
留下的脏数据会被逐块自动回收，烧录不需要 `erase_region` 前置。emutest 用
`df`（应报 3670016）和 `dd` 写 320 KiB（80 块，超出镜像内空闲块）再逐字节
读回校验来守护这条路径。

**真机验证（2026-11-03，4 MiB Macronix）**，六步全过：`df` 报到 3,670,016；
320 KiB 与 2.9 MiB（95% 满，尾块距分区顶 168 KiB）写入后全量校验通过、跨复位
不变；满盘再写时第 41 块 `write` 返回 −1，文件回落到 0 字节且 `df` 的 used 不变
（没有半截写入、没有泄漏块）；**重烧 300 KiB 小镜像后** `df` 回到 131,072、
`ls` 只剩 readme.txt，紧接着同样大小的写入照样成功并校验通过——尾部旧数据
既没干扰挂载，也没有因为"没擦就写"而出错位。

**写性能（2026-11-03）**：3 MiB / 740 块约 60 秒（≈50 KiB/s）。`/proc/lfs`
的计数器给出分布：5,937 次 prog（3,039,744 字节）+ 743 次块擦除，**擦除占掉
一半墙钟**。曾实现 512 B 写合并缓存来吸收 4 字节的 CTZ 指针更新，实测一个字节
都没攒到（littlefs 的 `prog_size` 等于设备扇区，prog 全是 512 对齐直写），已删除；
留下的计数器就是这条结论的证据。

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

**C3 的内核堆回到 32 KiB（2026-11-03）**：arena 曾按"`.bss` 尾到第一个任务窗口
之间空着约 50 KiB"配成 64 KiB，于是内核长到 175.8 KiB / 176 KiB 预算，距
`PLAT_APP_BASE`（`0x403AC000`）只剩 **240 字节**。`free` 给出的 peak 说明这是白留：
写完 3 MiB 文件之后，堆峰值也只有 11,648 字节。改回 `heap.c` 的默认 32 KiB 后：

```
kernel       147904  (144 KiB)   ← 原 179,984
tasks        196608  (192 KiB)
unused        34128  ( 33 KiB)   ← 原 2,048
```

同时给 `board/esp32c3/kernel.ld` 补了 `ASSERT(__kernel_end <= 0x403AC000)`（rp2040
早就有）。在此之前，内核 `.bss` 再长 240 字节可以链接成功，运行期表现为 pid 1 的
映像被内核全局变量覆盖——症状是"用户程序随机崩"，没人会往链接脚本上想。

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

### 电源：关机与复位（跨板）

init 任务退出走 `plat_shutdown()`；shell 的 `reboot` 走 `SYS_REBOOT` →
`plat_reboot()`。两者都是 noreturn，都先 `console_shutdown()` 把控制台排空——
复位不会替你冲刷发送 FIFO，最后一行的尾巴会被吃掉（模拟器里的 16550 尤其明显，
它确实有个输出队列）。

| 板 | shutdown | reboot | 状态 |
|---|---|---|---|
| x86 | ACPI `0x604`/`0xB004`/`0x4004` | 键盘控制器 `0x64 <- 0xFE` | smoke 跑通（复位后出现第二次 banner） |
| qemu-virt | SiFive test `0x100000 <- 0x5555` | 同设备 `0x7777` | smoke `-AllowReboot` 跑通两次 boot |
| qemu-mps2 | 停机（无对应设备，测试认最后一行） | `AIRCR.SYSRESETREQ` | 未单独验 |
| esp32c3 | 停机（未做 deep sleep） | `RTC_CNTL_OPTIONS0(0x60008000) bit31` = `SW_SYS_RST` | **真机验过** |
| rp2040 | 停机 | `AIRCR.SYSRESETREQ` | 未验（Pico 还没有控制台） |

C3 这条是这一轮唯一在真机上验过的：写之前先等 FIFO 排空、再空转几毫秒（最后
一个字符还在移位寄存器里，115200 下 87 µs），然后写那个只写位。证据是 ROM 自己
打印的 `rst:0x3 (RTC_SW_SYS_RST)`——它和主机拉 EN 的 `POWERON` 不是一回事，所以
`scripts/esp-c3/accept.py` 直接断言这行字符串；`Saved PC` 落在 `plat_reboot()`
的自旋里，也印证复位确实发生在写完之后。ARM 两个端口用的是 `AIRCR` 的
`SYSRESETREQ`（`arch/arm/scs.h`），写法照规范但还没有硬件可验。

## 9. 行为一致性契约

| 项 | 约定 |
|---|---|
| 系统调用号 | `include/tos.h`，0–29 |
| 参数寄存器 | x86 `ebx/ecx/edx`；rv32 `a0/a1/a2`；arm `r0/r1/r2`，由 trap 入口统一映射 |
| 返回值 | 同上，负数表示错误 |
| 控制台 | `\n` 自动转 `\r\n`；退格回显 `\b \b` |
| 文件系统 | littlefs，块大小 4096，相同文件树 |
| ELF 错误文案 | 相同 |
| exec | spawn，返回子任务 pid；`wait(pid)` 取退出码 |
| 调度 | 时间片轮转，50 ms；只在用户态帧上抢占 |
| 时钟 | `clock_ns()` 来自自由运行计数器，单调；`cpus` 字段是编译期 CPU 数 |
| 描述符 | 0/1/2 是控制台，其余从 3 起分配；`FD_NONE/FD_CONSOLE/FD_FILE/FD_PIPE_R/FD_PIPE_W` 五种 |
| 管道 | 空且仍有写者 → 阻塞；写者全关 → `read` 返回 0（EOF）；读者全关 → `write` 返回 -1（EPIPE）；缓冲 2 KiB |
| 内存保护 | x86 无（平坦）；rv32 PMP：code RX / data+heap RW / stack RW / 其余拒绝；arm MPU：窗口 RW+XN / code RO+可执行 / 栈下 16 KiB 护栏；内核读用户内存不受影响 |
| 异常隔离 | 只杀当前任务，其余继续跑 |

**`crash` 测试已经改写（P0 完成）。** 原先它靠写 `0xdeadb000` 触发缺页；关闭分页后没有缺页，
三平台行为无法一致。现在改为执行**非法指令**（x86 `ud2`、rv32 `.word 0`、arm `udf #0`），
三个架构上都产生“非法指令”异常，由内核终止当前任务。

验证方式：`tostest compat` 对每个目标启动对应 QEMU，喂相同命令序列，
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

## 13. 多核（SMP）

目标不是“跑满多核”，而是**在同一份内核上让第二个 hart 也能跑用户任务**，为 RP2350
这种双核 MCU 铺路。验收标准就一条：`smp` 程序的两个 worker 同时报出不同的
`getcpu()`，且 `ps` 里两个任务同时处于 `run`、`c` 列分别是 0 和 1。

### 13.1 哪些是 per-CPU，哪些是共享

这是整件事的骨架。判断标准是“它描述的是这台机器，还是这一个核”：

| | 归属 | 原因 |
|---|---|---|
| `current` / `rr` / `quantum` / `need_resched` | **per-CPU**（`cpu_t`） | “我正在跑谁”只对本核有意义 |
| 内核栈 | **per-CPU** | 陷入时从用户栈切过来，必须是自己的 |
| `mtvec`、`mie.MTIE`、`mtimecmp` | **per-CPU**（硬件就是 per-hart CSR） | 一核装了比较值，另一核什么也不知道 |
| 进程表 `procs[]` | **共享** | 只有一条运行队列，两个核都从这里取任务 |
| 控制台 / littlefs / tty 行缓冲 | **共享，但只由 0 号 hart 驱动** | 一个设备一个 FIFO，两个核抢着读会把一行拆成两半 |
| `g_ticks`（调度 tick 计数） | **共享，只由 0 号 hart 递增** | 两核都加就变成 200 Hz |
| PMP 条目（`pmpcfg0`） | **per-CPU** | 内存保护是“正在跑的任务”的属性 |

### 13.2 用宏把单核代码原样搬过去

`process.c` 里 `current`、`quantum`、`need_resched` 三处原来是 file-static。
改成 `cpu_t` 的成员之后，如果逐处改写成 `cpu_self()->cur`，会有 40 多个调用点要动，
而且以后每一行新代码都得记得不能直接用 `current`。所以反过来做：

```c
#define current        (cpu_self()->cur)
#define rr             (cpu_self()->rr)
#define quantum        (cpu_self()->quantum)
#define need_resched   (cpu_self()->need_resched)
```

于是调度器那几百行**一行都没改**，语义自动变成 per-CPU。代价是 `cpu_t` 的成员
不能叫 `current`（否则 `cpus[i].current` 会被宏展开两次），所以字段叫 `cur`。

`cpu_self()` 就是 `&cpus[plat_cpu_id()]`，rv32 上是 `csrr mhartid`，一条指令。

### 13.3 从 hart 起步（`arch/rv32/start.S`）

所有 hart 都从 `_start` 进来。第一件事必须是分流，**在清 `.bss` 之前**：

```asm
csrr    t0, mhartid
bnez    t0, .Lsecondary
```

否则两个 hart 同时把 `.bss` 清零，会把对方正在用的内存擦掉。从 hart 进来后：

1. 自旋等 `cpu_release == 0x534D5031`（`"SMP1"`，`kernel/cpu.c` 定义）。
   用 magic 而不是“启动完成”标志，是因为标志本身也在 `.bss` 里——它被清零这件事
   和从 hart 读它这件事本身就是竞争。magic 在 `cpu_init()` 最后一步才写，
   那时进程表、控制台、定时器都已就绪。
2. 装自己的栈：`sp = __kernel_stack_top - mhartid * 32K`。
3. 装自己的 `mtvec`（`trap_vectors_h1`）。
4. `call cpu_secondary_main`，永不返回。

### 13.4 两张 trap 向量表

`mtvec` 是 per-hart CSR，而 trap 入口必须知道“该切到哪个栈”。所以入口被写成宏：

```asm
.macro TRAP_ENTRY name, kstack
\name:
    csrw mscratch, t0
    ...
    la   t0, \kstack        /* 只有这一行按 hart 不同 */
    ...
.endm

    TRAP_ENTRY trap_entry_h0, __kernel_stack_top
    TRAP_ENTRY trap_entry_h1, __kernel_stack_top - (1 << KSTACK_SHIFT)
```

`\kstack` 是绝对表达式，链接器把它折进 `la`，所以整个切换只用掉一个暂存寄存器
（`t0` 先存进 `mscratch`）。两张 256 字节表背靠背放着，`pmp_open_trap_vector()`
那条 NAPOT 规则一次覆盖两张——否则从 hart 一进 trap 就 `instruction access fault`
（C3 的 esp-riscv 核会用**旧特权级**做 PMP 检查，这个坑已经踩过）。

### 13.5 锁

一把 `spin_t`，`kernel/cpu.h`。单核上 `spin_lock()` 是个 `if (cpu_count > 1)`，
什么也不做——内核在系统调用里不会被切走，本来就没有需要互斥的东西。

多核上实现在 `arch/rv32/platform.c`：

```c
do {
    __asm__ volatile("amoswap.w.aqrl %0, %1, (%2)"
                     : "=r"(got) : "r"(1), "r"(v) : "memory");
} while (got != 0);
```

`.aqrl` 才是关键：swap 本身只是 test-and-set，让临界区对另一个核可见的是那两个
ordering 位。释放用 `fence rw, rw` + 普通 store。

这把锁只在 `rv32imac`（`TOS_MAX_CPUS > 1`）编进去；单核构建用 `kernel/cpu.c`
里的 weak 空实现，所以 `rv32imc` 的 C3 和没有 LDREX/STREX 的 Cortex-M0+ 都不用假装
自己有原子指令。

**锁的边界**：`proc_lock` 保护“扫一遍进程表 + 改状态”，绝不跨任何会阻塞的调用。
调度器在 `sched_idle()` 之前必须把锁还回去，否则空闲的那个核会抱着锁去 `wfi`。

**第二把锁：`fs_lock`（`kernel/ramdisk.c`）**。littlefs 的共享面比“磁盘”大：
`cfg` 里那份 read/prog/lookahead 缓存、`ramdisk.c` 的 4 KiB bounce 缓冲、句柄表和
readdir 快照全都是全局的。所以互斥的粒度不是块，是**一次 API 调用**——两个核各读
一个文件，会互相冲掉对方的缓存块。

- 单核上它同样是空操作，理由和 `proc_lock` 一样：任务只在用户态或阻塞点丢 CPU，
  没有第二个任务能看到“半个 lfs 调用”。这个结论依赖两条前提，写在 `ramdisk.c`
  的注释里：tick 落在内核态只记账不切换；锁内不阻塞（将来做阻塞读要先放锁）。
- 写法是“内部函数 + 包装”（`count_locked` / `ramdisk_count` 那种形状），因为
  `list_name` 要调 `name`、`list_count` 要调 `count`，而 spin 锁不可重入。入口
  漏掉一个会链接不上，比在每个函数里手写 lock/unlock、某个 `return` 忘了放可靠。
- 顺带把 readdir 的名字改成**拷贝进调用者的缓冲**（`ramdisk_list_name(dir, i, buf,
  cap)`、`devfs_dir_name(...)`）。原来是返回静态缓冲的指针——锁一放开，另一个核
  就能在调用者 memcpy 之前把它改写，等于没加锁。

### 13.6 定时器：一份时钟，两个 tick

每个 hart 都需要自己的 tick（它才是抢走本核 CPU 的东西），但“现在几点”是全局的。
所以：

- `plat_timer_start_cpu()` 给新 hart 写自己的 `mtimecmp`、打开自己的 `mie.MTIE`；
- `plat_timer_tick()` 里 **只有 `plat_cpu_id() == 0` 才 `g_ticks++`**，两个核都重装
  自己的比较值。

写错这一处的症状很好认：`uptime` 走两倍。实测两个 hart、9 秒真实时间，
`uptime` 报 9.102 s。

### 13.7 控制台

两条规则：

1. **只有 0 号 hart 调 `tty_poll()` / `console_echo_tick()`**。它是 `process_tick()`
   里 `if (cpu_id() == 0)` 的全部内容。理由是一个 16550 只有一个 FIFO 和一份行缓冲，
   两个核抢着 drain 会把一行输入劈成两半（此时两个 shell 各拿到半行）。
2. **输出流一把 `out_lock`**，粒度是“一次 `kprintf` / 一次 `write()`”。`printf` 和
   `puts` 在用户库里都是先格式化进缓冲再一次 `write(1, …)`，所以一次 `printf`
   的输出不会被另一个核切开。分开的 `write()` 之间仍然可以交错——这在任何 SMP
   系统上都一样，而 `out_char()` 原有的“行主换人时补一个换行”逻辑正好让交错读得懂。

### 13.8 调度器里两个 SMP 才有的坑

**`other_cpu_busy()`。** 单核的 `sched_switch()` 在“没有 ready 且没人阻塞”时
`panic("no runnable process")`。两核上这会误判：空闲 hart 找不到 ready 任务、
也没人阻塞，但另一个 hart 正跑着 shell（状态 `RUNNING`，而 `RUNNING` 的任务
别的核不碰）。所以 panic 前还要看一眼别的 hart 是不是在跑东西。

**`make_ready()` 的锁。** 唤醒者改状态、调度器读状态，两个核可能同时看同一个任务。
状态存进锁里，至少保证“一个赢”；否则一个已经被调度器看成 `READY` 的任务又被写入
返回值，那个返回值就丢了。

**`ST_NEW`：认领一个空槽。** `proc_create()` 要先解压、加载 ELF、建初始栈，中间
几百行不能一直抱着锁。所以 `proc_alloc()` 在锁内把槽位标成 `ST_NEW`（“有人占了，
但永远不可调度”），`proc_create()` 最后一步才在锁内改成 `ST_READY`。没这一步，
两个核同时 `exec` 会选中同一个 `proc_t`，两个任务叠在同一块窗口和同一份栈上。
顺带两个坑：

- `proc_create()` 原来在中间 `memset(p, 0, sizeof(*p))`，而 `ST_FREE == 0`——
  那一行会把刚做的认领擦掉。`memset` 因此移进了 `proc_alloc()` 的锁内。
- 每个失败路径（ELF 不合法、窗口不够、栈不够）都必须 `proc_retire(p)` 把槽还回去，
  否则它卡在 `ST_NEW`，现象是“进程表满了”而机器上什么都没跑。

**关机不是调度失败。** `init` 退出时，另一个核可能正好发现“没有 ready、没人阻塞”，
于是 `panic("no runnable process")`——在一台**按指令正常关机**的机器上。所以
`process_exit()` 在释放 init 槽位**之前**（锁内）先置 `sys_down`，调度器看到空队列
时再读这个标志：关机中就 `wfi` 等着，别 panic。实测这一条是十轮压力测试里唯一
还能复现的 panic（`-commands "... ,exit"` 跑 10 次大约中 2 次）。

**窗口位图和 pid 也是共享的。** `win_used` 是一个 32 位位图，两个核同时
`win_alloc/win_free` 就是经典的读-改-写竞争，丢了位以后症状迟得多才出现
（“out of task windows” 但明明有空位，或者两个任务共用一块窗口）。`next_pid++`
同理——重号会让 `wait(pid)` 收到别人的退出码。两者都挪进了 `proc_lock`。

### 13.9 还没做的

- **RP2350 / RP2040 的 core1**：`board/rp2040` 还是 `PLAT_MAX_CPUS = 1`。
  需要的是 SIO 自旋锁（`SIO_SPINLOCK0`）+ 把 core1 从 bootrom 的 `wfe` 循环里
  拉起来（写 `SIO_FIFO` 或走 PSM）。`plat_spin_acquire()` 换成 SIO 版本即可，
  其余层不用动。
- **任务亲和性**：现在是一条队列、谁有空谁拿，任务会在核之间迁移（`smp` 程序会
  报 `cpu 1 then 0`）。真要绑核，`proc_t` 加一个 `affinity` 字段、`sched_pick()`
  过滤一下就行，但那是策略，先不做。
- **TLB / 缓存一致性**：没有 MMU 也没有 D-cache，所以这一层暂时不存在问题。
  RP2350 有缓存，到时候 `pipe`/`console` 的缓冲要过 `fence`。


## 14. 设备文件（/dev 与 /proc）

外设先是 C API（`SYS_GPIO` / `SYS_ADC`），够程序用，不够 shell 用：`cat` 一个
ADC 通道、`echo 1 >` 一个引脚，本来都该是一行命令，结果每个都要单独编一个程序。
这一节记的是"设备即文件"这条路的实现，以及为什么它不是 sysfs。

### 14.1 描述符本来就已经是 tagged union

`file_t` 在加管道那一轮就变成了 `{type, fh, size, pos, pipe}` 的判别联合，
`process_read/write` 本来就是 `switch (f->type)` 分派。所以设备文件不是新架构，
是把已有的特例表化：

```c
enum { FD_NONE, FD_CONSOLE, FD_FILE, FD_PIPE_R, FD_PIPE_W, FD_CHRDEV };
```

`FD_CONSOLE` 是第一个字符设备，只是它靠 `fd == 0` 这个魔法数字认领，而不是靠表。
加了 `FD_CHRDEV` 之后，console 反而成了"没有 dev_ops 的特例"。

### 14.2 表在 boot 时填满，之后只读

```c
struct dev_ops {
    int  (*open)(const struct dev *d, void **priv);
    int  (*read)(void *priv, uint32_t off, void *buf, uint32_t len);
    int  (*write)(void *priv, uint32_t off, const void *buf, uint32_t len);
    int  (*ioctl)(void *priv, int cmd, int arg);
    void (*close)(void *priv);
};
```

`devfs_register()` 只在 `kmain` 里调用，表项 64 个、名字放在静态池里（`gpio4`
这种名字要靠 `ksnprintf` 现拼，不能指向栈）。运行期没有任何写入，所以 SMP 上
`open()` 走这张表**不需要锁**——这是把"注册是启动行为"当契约用，省掉一把锁。

没写驱动的板子注册 0 个设备：`/dev/gpio0` 打不开，和 `gpio_count() == 0` 是同一
个事实的两种问法。`procfs` 注册三个伪文件，`periph` 按 `plat_gpio_count()` /
`plat_adc_count()` 循环注册。

### 14.3 数据面走文本，配置面走 ioctl

```
cat /dev/adc2        → read()  → "2048\n"
echo 1 > /dev/gpio4  → write() → 解析首个非空白字符，自动配成输出
gpio dev mode 4 out  → ioctl(fd, GPIO_OP_MODE, TOS_PIN_OUT)
```

三个设计决定：

1. **read 是快照，不是流**。`off != 0` 直接返回 0（EOF），否则 `cat` 会读同一
   个电平永远不退出。轮询是 C API 的事（`ioctl(fd, GPIO_OP_READ)` 不消耗偏移）。
2. **write 隐含配方向**。拒绝写直到 ioctl 设过方向，会让 `echo 1 > /dev/gpio4`
   失败——而那个引脚本来就是调用者的，写它只可能是想输出。
3. **配置面必须有 ioctl**。方向、上拉、ADC 的 vref/bits 既不是字节读也不是字节写。
   复用 `tos.h` 里已有的 op 码，`SYS_IOCTL` 只是一个转发，代价 30 行。

### 14.4 为什么不是 sysfs

`/sys` 的复杂度 = VFS 的复杂度 + 对象模型的复杂度 + 文本序列化的复杂度：
要有 kobject 树、每属性一对 `show/store`、`readdir` 动态枚举、每节点权限。
而 TinyOS 三个前置件都没有——没有 uid/gid（权限位形同虚设）、没有对象生命周期
（设备和进程表都是编译期定长）、没有 VFS（`open` 现在直通 `ramdisk_open`）。
`/dev` + ioctl 覆盖了 sysfs 想解决的问题的 95%，成本是它的 1/15。
判据是"有没有大量结构相同、数量运行期才知道的对象"，现在离得很远。

### 14.5 重定向要靠描述符继承

`echo hi > f` 的实际形状是：shell 打开 f、`dup2(fd, 1)`、`exec("echo")`。也就是说
**fd 1 必须能跨 exec 交给子进程**，否则重定向到任何程序都不生效。原来是拒绝的
（`FD_FILE` 不继承，理由是"句柄带读位置，也没有路径可重开"）。现在 `file_t`
存一份 `path`（堆上拷贝），继承时：

- `FD_FILE`：按相同访问模式重开（`ramdisk_open` / `ramdisk_open_rw`），再
  `ramdisk_seek` 到父进程的位置——新句柄从 0 开始，位置要靠 seek 重建。
- `FD_CHRDEV`：调 `ops->open` 拿自己的 per-open 状态（`/proc` 的快照缓冲不能被
  两个描述符 free 两次）。
- `FD_PIPE_*`：引用计数；`FD_CONSOLE`：无事可做。

这套规则只有一份实现（`fd_clone`），`dup2()` 和子进程继承共用——两处各写一遍
是这类代码最常见的漂移来源。

### 14.6 顺手修掉的两个真 bug

- **x86 的内核栈池与 `KSTACK_BYTES` 不一致**（16 KiB vs 32 KiB）。栈守卫按
  `__kernel_stack_top - KSTACK_BYTES` 写，结果落在栈下 16 KiB、进程表中间。
  症状是 `ps` 里多一行 `pid 0, state "free"` ——它的 `state` 其实是 `0x5354414B`
  （"STAK"）。修法：x86 的 `.space` 改成 32768，并在 `cpu_init()` 里断言
  `KSTACK_POOL_BYTES == KSTACK_BYTES * PLAT_MAX_CPUS`，把"三处手工同步"变成
  启动时报错。
- **内核堆没有锁**。`kmalloc/kfree` 是共享空闲链表上的 first-fit 遍历，SMP 下
  exec() 和 `/proc` 的 open() 并发就会重复分配同一块。加 `spin_t`（单核退化为
  空操作，代价为零），中断不关——因为目前没有任何中断处理路径分配内存。

### 14.7 目录是算出来的，不是存出来的

设备表现在有两套用法：按名字查（`open`）和按目录列（`ls`）。目录没有独立的数据
结构——注册名本身就带路径，`dev/gpio4` 的父目录就是斜杠前的那一段：

- `/dev` 的项 = 表里形如 `dev/<x>`、且 `<x>` 不含斜杠的名字，去掉前缀就是目录项；
- `/` 的项 = 固定命名空间（`dev`、`proc`）加上表里其余首段去重。

`devfs_dir_count()` 用 `-1` 表示"这不是 devfs 目录"，`0` 表示"是我的、但是空的"。
这个区分是必须的：前者让 `readdir` 继续去问 littlefs，后者必须直接回答 EOF，
否则 `ls /dev` 会掉进 littlefs 报 "no such directory"。

两个命名空间无条件存在，哪怕一块板子一个设备都没注册（qemu-virt 就是）。"这块板
没有 GPIO" 是关于板子的事实，不该表现成"`cd /dev` 在 C3 上能用、在 virt 上不能"。

`/` 是唯一的混合目录：先出 `dev`、`proc`，再出 littlefs 的项。所以偏移的换算放在
`SYS_READDIR` 里——那是唯一同时看得见两个枚举器的地方。

`stat` 也跟着分了一层（`path_stat`）：`/dev`、`/proc` 报目录，设备节点报新类型
`TOS_FTYPE_CHR`（`ls` 打 `c` 和 `<dev>`，而不是 "0 bytes"）。`SYS_CHDIR` 用同一个
helper——能 `ls` 却不能 `cd` 的目录等于半个目录。

### 14.8 还没做的

- **阻塞型设备**：GPIO/ADC 都是即时返回。等按键的设备需要 `read` 返回
  "would block"，由内核统一挂起——现在的 `ST_WAIT_INPUT` / `ST_WAIT_PIPE` 是
  两条独立路径，第三条该收敛成一个通用机制。
