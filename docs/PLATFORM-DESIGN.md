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
void     plat_mem_init(void);
paddr_t  plat_page_alloc(void);      /* 4 KiB，失败返回 0 */
void     plat_page_free(paddr_t p);
int      plat_user_range_ok(vaddr_t va, size_t len);

/* ---- 用户态进出 ---- */
void plat_set_kernel_stack(vaddr_t top);
void plat_enter_user(trapframe_t *tf) __attribute__((noreturn));
int  plat_fault_is_user(trapframe_t *tf);
void plat_panic_halt(void) __attribute__((noreturn));
```

**没有 `plat_as_*`** —— 平坦地址空间下地址空间抽象是纯开销。
`trapframe_t` 由 `arch/*` 定义，core 只持有不透明结构。

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

**关键后果：用户程序必须按目标板的 `APP_BASE` 链接。**
无 MMU + 无重定位时，二进制是板级相关的。构建系统需要按 board 生成用户程序，
而不是一套二进制通吃。将来若要多板共用，再引入 PIE + 重定位。

## 5. 特权级

三个架构都提供“不能执行特权指令”，但都不提供内存保护：

| | 用户态 | 陷入方式 | 内核栈 |
|---|---|---|---|
| x86 | ring 3（分段平坦，**CR0.PG=0**） | `int 0x80` | TSS `esp0`，每任务 |
| RV32 | U-mode | `ecall` | 每任务（可选单栈） |
| ARM | Thread/unprivileged | `svc` | PSP（硬件自动切换），每任务 |

x86 关闭分页后：内核与用户共享 4 GiB 平坦空间，ring 3 仍能阻止 `cli/hlt/out` 等特权指令，
系统调用仍然走中断门。这与 C3/RP2040 的能力边界一致。

## 6. 任务模型

```c
typedef struct task {
    int         used;
    int         pid;
    int         state;
    trapframe_t frame;        /* 用户寄存器现场 */
    vaddr_t     user_sp;
    vaddr_t     kstack_top;   /* 每任务内核栈（x86 必需，ARM 用 PSP） */
    vaddr_t     brk, brk_end;
    fd_t        fds[MAX_FDS];
    struct task *parent;
} task_t;
```

- 没有地址空间字段：所有任务共享同一平坦内存。
- 语义与现在一致：`exec` 同步，父任务阻塞，退出码返回父任务；用户态异常只杀当前任务。
- 调度：当前为调用栈式；接口预留 `task_switch()`，后续可加抢占。

## 7. ELF 与重定位

- 保留标准 ELF32；`e_machine` 由 arch 校验（`EM_386=3`、`EM_RISCV=243`、`EM_ARM=40`）。
- 加载器工作变成“把 `PT_LOAD` 拷到 `p_vaddr`，清零 `.bss`”，不再有页映射。
- 链接地址 = `APP_BASE`，由板级链接脚本传入。
- 可选 LZ4TOS 信封：解压后仍是标准 ELF，三平台共用。
- 重定位：暂不做。若将来需要多程序共存或跨板二进制，再打开 `-fPIE -pie`，
  实现 `.rela.dyn`（RV32）/ `.rel.dyn`（ARM）。接口预留 `elf_apply_relocs()`。

## 8. 启动链

### x86（现状，需改造）

```
BIOS -> MBR(stage1.S) -> 内核 @0x10000 -> kmain
  改造：去掉 paging_init / CR3 切换；APP_BASE 移到 RAM 内
```

### QEMU virt（rv32，参考实现）

```
qemu-system-riscv32 -machine virt -bios none -kernel tinyos.elf
  入口 0x80000000（M 模式）
  UART 16550 @0x10000000
  CLINT @0x02000000
  RAM 128 MiB @0x80000000
  块设备：内存盘（先）-> virtio-blk（后）
```

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
| 系统调用号 | `include/tos.h`，0–11 |
| 参数寄存器 | x86 `ebx/ecx/edx`；rv32 `a0/a1/a2`；arm `r0/r1/r2`，由 trap 入口统一映射 |
| 返回值 | 同上，负数表示错误 |
| 控制台 | `\n` 自动转 `\r\n`；退格回显 `\b \b` |
| 文件系统 | littlefs，块大小 4096，相同文件树 |
| ELF 错误文案 | 相同 |
| exec | 同步，退出码返回父任务 |
| 异常隔离 | 只杀当前任务 |

**`crash` 测试必须改写。** 现在它靠写 `0xdeadb000` 触发缺页；关闭分页后没有缺页，
三平台行为无法一致。改为执行**非法指令**（x86 `ud2`、rv32 `.word 0`、arm `udf #0`），
这样在三个架构上都产生“非法指令”异常，由内核终止当前任务。

验证方式：`scripts/compat.ps1` 对每个目标启动对应 QEMU，喂相同命令序列，
比较归一化输出（去掉时间戳和 pid）。

## 10. 迁移阶段

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P0** | x86 去分页 + 抽 `platform.h`；`crash` 改非法指令 | smoke 原样通过（除 crash 提示） |
| P1 | `arch/rv32` + `board/qemu-virt`：串口、定时器、trap | 打印 banner |
| P2 | RV32 ELF 直接加载 + syscall + 内存盘 littlefs | 跑 `hello` |
| P3 | `arch/arm` + `board/qemu-mps2`：异常、PSP、SysTick | 跑 `hello` |
| P4 | ESP32-C3：ESP 镜像 + SPI flash | 真机 `hello` |
| P5 | RP2040：boot2 + UF2 + SRAM 运行 | 真机 `hello` |
| P6 | 可选：抢占调度、PIE/重定位 | — |

P0 是最容易被低估的一步：**它会让 x86 失去现有的进程隔离**，
`crash` 的行为也会改变。这是刻意的取舍，不是回归。

## 11. 风险清单

| 风险 | 影响 | 缓解 |
|---|---|---|
| 无内存保护 | 用户程序可破坏内核 | 文档明确；`plat_user_range_ok` 只做边界检查 |
| 用户二进制与板级绑定 | 跨板需重新链接 | 构建系统按 board 输出；将来 PIE |
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
