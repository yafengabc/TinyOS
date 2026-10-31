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
void plat_panic_halt(void) __attribute__((noreturn));

/* ---- trap frame 视图（架构无关） ---- */
void     plat_frame_get_syscall(const trapframe_t *tf, syscall_t *sc);
void     plat_frame_set_ret(trapframe_t *tf, uint32_t val);
```

**没有 `plat_as_*`** —— 平坦地址空间下地址空间抽象是纯开销。
`trapframe_t` 由 `arch/*` 定义，core 只持有不透明结构；syscall 号与参数通过
`syscall_t {nr, a0, a1, a2}` 暴露，core 不直接读 `eax/ebx/ecx/edx`。
用户指针校验也不再是平台函数，而是 `process_user_ok(addr, len)`——只做窗口范围检查。

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
| ESP32-C3 | 400 KiB @0x3FC80000 | `0x3FC90000` | 应用窗口 ≤ ~200 KiB |
| RP2040 | 264 KiB @0x20000000 | `0x20010000` | 应用窗口 ≤ ~120 KiB |

**关键后果：用户程序以 PIE（ET_DYN）+ 运行时重定位加载，跨板分发同一份二进制。**
加载器把镜像放到各板的 `APP_BASE`，重定位表（`.rel/.rela`）修正内部绝对地址，
同一份用户二进制可在 QEMU 与不同 `APP_BASE` 的真机间通用，无需按板重新链接。

## 5. 特权级

三个架构都提供“不能执行特权指令”，但都不提供内存保护：

| | 用户态 | 陷入方式 | 内核栈 |
|---|---|---|---|
| x86 | ring 3（分段平坦，**CR0.PG=0**） | `int 0x80` | TSS `esp0`（共用一层 16 KiB 内核栈） |
| RV32 | U-mode | `ecall`（M 模式陷入） | 单一 32 KiB 内核栈，U 模式陷入时切换（P2 已实现） |
| ARM | Thread/unprivileged | `svc` | PSP（硬件自动切换），每任务 |

x86 关闭分页后：内核与用户共享 4 GiB 平坦空间，ring 3 仍能阻止 `cli/hlt/out` 等特权指令，
系统调用仍然走中断门。这与 C3/RP2040 的能力边界一致。

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
- **PMP**：U 模式每个访问都要过 PMP，未配置则第一条用户指令就 fault。P2 用一条
  NAPOT 全空间 RWX 条目（`pmpaddr0=0x3FFFFFFF`, `pmpcfg0=0x1F`），与"平坦模型
  不做内存保护"的取舍一致。
- **内核栈**放在 `start.S` 的 `.bss` 里（32 KiB），`start.S` 清零 `[__bss_start,
  __bss_end)` 时一并清零，和 x86 的做法一致。
- **缺的软除法**：RV32 没有 64 位移位指令，clang 会为 `>>`/`<<` 生成
  `__lshrdi3/__ashldi3/__ashrdi3` 调用，`kernel/libgcc.c` 补齐了这三个（用两个
  32 位半字实现，避免递归调用自己）。

### QEMU mps2（ARM 参考）

```
qemu-system-arm -machine mps2-an385 -kernel tinyos.elf
  Cortex-M3，适合先验证异常/PSP/SysTick，再接 RP2040
```

### ESP32-C3

```
ROM bootloader -> ESP 镜像（自定义头/段表）-> 内核
  UART0 @0x60000000
  SRAM 400 KiB @0x3FC80000
  SPI flash 4 MiB：需要读/写/擦驱动
```

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
| P3 | `arch/arm` + `board/qemu-mps2`：异常、PSP、SysTick | 跑 `hello` |
| P4 | ESP32-C3：ESP 镜像 + SPI flash | 真机 `hello` |
| P5 | RP2040：boot2 + UF2 + SRAM 运行 | 真机 `hello` |
| P6 | 可选：PIE/重定位加固、优先级、每子任务退出码队列 | — |

P0 是最容易被低估的一步：**它会让 x86 失去现有的进程隔离**，
`crash` 的行为也会改变。这是刻意的取舍，不是回归。

## 11. 风险清单

| 风险 | 影响 | 缓解 |
|---|---|---|
| 无内存保护 | 用户程序可破坏内核 | 文档明确；`process_user_ok` 只做窗口范围检查 |
| 用户二进制跨板分发 | 依赖加载器重定位正确性 | ELF32 ET_DYN + `.rel/.rela`，elf.c 校验目标窗口 |
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
  arm/  start.S exc.S pmm.c systick.c uart.c block_ram.c
board/
  pc/        layout.h
  qemu-virt/ layout.h init.c
  qemu-mps2/ layout.h init.c
  esp32c3/   layout.h init.c flash.c image.h
  rp2040/    layout.h init.c boot2.h
```
