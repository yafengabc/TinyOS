# TinyOS

一个用 **C + 汇编** 写的极简 32 位操作系统，目前有 **x86（PC/QEMU i386）**、
**RV32（QEMU virt）** 和 **ARM（QEMU mps2-an385，Cortex-M3）** 三个可运行目标，
配套的构建/运行工具用 **Go** 写。
它的核心能力是：从 **littlefs** 镜像里**动态加载 ELF 可执行文件**，在用户态
（x86 ring 3 / RV32 U-mode / ARM Thread+PSP 非特权 + MPU）里运行，
程序可以 `exec` 另一个程序、读写文件、申请堆内存；程序崩溃只会杀掉自己，不会拖垮内核。

```
x86 : BIOS -> 自写 MBR 引导扇区 -> 32 位保护模式 -> 内核
       -> GDT/IDT/TSS（平坦模型，不启用分页）+ 定时器 + 键盘
       -> 挂载 littlefs（ATA 分区），加载 /bin/sh (ELF32) -> 进入 ring 3
RV32: qemu-system-riscv32 -bios none -> 内核 @0x80000000（M 模式）
       -> trap 向量 + CLINT 定时器 + 16550 串口 + PMP 分段（每任务 code/data/stack）
       -> 挂载内嵌 littlefs 镜像（RAM 盘），加载 /bin/sh -> mret 到 U-mode
ARM : qemu-system-arm -machine mps2-an385 -> 向量表 @0（Cortex-M3）
       -> Thread/PSP 跑任务、Handler/MSP 跑内核 + SysTick + CMSDK UART0
       -> 挂载内嵌 littlefs 镜像（RAM 盘），加载 /bin/sh -> 异常返回进入用户任务
```

三条链跑的是**同一套内核代码**（`kernel/`）和**同一批用户程序**：程序是 PIE，
加载时按板级 `APP_BASE` 重定位，所以同一份 `bin/hello` 在各目标上都能跑。

## 功能

- 自写 512 字节 MBR 引导扇区（int 13h LBA 扩展读盘，A20，进入保护模式）
- **ATA PIO 磁盘驱动**（28-bit LBA、轮询无 IRQ）：内核启动后用 `ata_read_sectors`
  /`ata_write_sectors` 直接读写磁盘，不再把文件系统整个拷进内存
- **平坦地址空间、不启用分页**（CR0.PG=0）：内核与用户程序共享一条 4 GiB 直线地址
  空间，`ring 3` + TSS 只提供特权级隔离、不提供内存保护——这与 MCU 目标
  （ESP32-C3 / RP2040）的能力边界一致
- **每任务窗口**：`exec` 用 ELF 的 `memsz` 算出镜像大小，加上栈和守卫洞、按粒度
  向上取整，再向分配器要这么多；分配器扫描进程表里的活窗口找第一段够大的空隙
  （活窗口本身就是空闲表，任务一死空隙自然合并）。镜像在底部、`sbrk` 堆在中部、
  栈在顶部，父子任务各占一块、互不覆盖。C3 上 `sh` 占 32 KiB、300 字节的小工具占
  16 KiB，同一个 192 KiB 能装 12 个这样的小任务。
- **栈和堆都按程序声明**：默认栈 8 KiB（C3 / RP2040），需要更深递归的程序在源码
  里写 `TOS_STACK(16 * 1024)`；堆不够的写 `TOS_HEAP(24 * 1024)`。两者都是 ELF
  note（`.note.tos` → `PT_NOTE`，type 1/2，可并存），`exec` 在算窗口之前读出来，
  同一台机器上不同声明的任务并存。栈必须是整除窗口粒度的 2 的幂（PMP 的 NAPOT /
  MPU 区域只能描述这种尺寸），堆没有这个约束（brk 是普通地址，TOR 编码），任意
  字节值都是合法下限；越界的请求 `exec` 拒绝并报出数字
- ELF32 加载器：解析 `PT_LOAD`、`p_filesz/p_memsz`，把镜像拷进任务窗口并清零 `.bss`
- **位置无关程序（PIE）**：用户程序链接为 `ET_DYN`（基址 0），内核加载时整体搬到
  该任务的窗口基址并应用 `.rel.dyn`/`.rela.dyn` 重定位。这样**同一份二进制可以在
  不同 `APP_BASE` 的板子上运行**（QEMU 与真实 MCU 共用）
- **LZ4 压缩的程序**：用户 ELF 在构建时压缩，内核内嵌约 420 字节的解压器在
  `exec` 时先解压再交给 ELF 加载器（`LZ4TOS` 信封，非压缩文件原样加载）
- 系统调用（x86 `int 0x80` / RV32 `ecall`）：
  `write/read/exec/open/close/readdir/getpid/sleep/uptime/sbrk/exit/create/wait/yield/ps/signal/reboot`
  外加 `clock/getcpu/gpio/adc/pipe/dup2/ioctl`
- **抢占式调度**：时间片轮转（1 ms）、`exec` 创建子任务、`wait` 收退出码、
  `read/wait/sleep` 阻塞而不忙等；只在用户态帧上抢占，内核调用不会被撕裂
- **真实时钟**：`uptime`/`clock_ns()` 读硬件自由运行计数器（CLINT `mtime`、
  ESP32-C3 的 TG0、Cortex-M 的 SysTick），不是数指令；纳秒级分辨率，单调不回退
- **外设框架**：`plat_gpio_*` / `plat_adc_*` 在 `kernel/periph.c` 是 weak 默认，
  板级强符号覆盖；`gpio` / `adc` / `reboot` 三个用户程序
- **设备即文件**：`/dev/gpioN`、`/dev/adcN`（`open`/`read`/`write`/`ioctl`，数据面走
  文本，配置面走 ioctl）与 `/proc/{ps,mem,uptime}`（只读快照，open 时生成）。
  于是 `cat /dev/adc2`、`echo 1 > /dev/gpio4`、`ps > f` 都能用，不必为每个外设
  再写一个程序；C API 仍然保留（`echo 1 >` 是三次 syscall 加一次路径查找）。
  目录是从设备表算出来的，所以 `ls /dev`、`cd /dev` 也成立（节点显示 `c` 和 `<dev>`）
- **管道（IPC）**：`pipe()` + `dup2()`，内核里是环形缓冲 + 读/写端引用计数，
  空管道阻塞、写者全关返回 EOF、读者全关返回 EPIPE；`cat readme.txt | wc` 就是它
- **Python 子集解释器**（`bin/py`，普通用户程序）：缩进分块、函数与递归、
  `list`/`dict`/`str`、异常按 Python 的样子报。自带 arena（引用计数回收），
  在 ESP32-C3 上跑百行脚本。详见下文「Python 子集解释器」
- **双核 SMP**（`-target rv32smp`）：per-CPU 运行队列游标与内核栈、`amoswap` 自旋锁
  （进程表 + 文件系统各一把）、每 hart 独立 `mtvec` 和 `mtimecmp`，两个 hart 同时跑
  用户程序
- 用户态异常（非法指令、GPF）只终止当前进程，其他任务继续跑
- **littlefs v2.9.3** 文件系统：构建期用 `mklfs`（同一份 littlefs 源码）打包到磁盘上的
  独立分区，运行时由内核通过 ATA 驱动挂载，支持子目录（`bin/`）、**真实写入**
  （`open/read/readdir` 全部走 littlefs，`prog/erase` 回写磁盘）。挂载时
  `block_count=0`（尺寸取自镜像超级块）+ `lfs_fs_grow` 扩到真实分区：烧录产物
  只需要 300 KiB，C3 上首启自动扩满 3.5 MiB 分区——v2.9 按树可达性判空闲、
  分配时先擦除，旧镜像尾部脏数据会被逐块回收，无需 erase_region
- `dd`：文件系统块级读写校验（`dd of=f bs=4096 count=80` 写图案、
  `dd if=f` 逐字节读回比对），扩容区和写入路径的回归就靠它
- `rm`：`unlink()` 的封装。目录和设备节点会被拒绝——littlefs 的
  `lfs_remove` 对目录是**递归删除**，`rm /bin` 不能真的把 shell 删掉；
  正在被打开的文件也拒绝，否则句柄会继续往已经不在树上的块里写
- `/proc/lfs`：块设备调用计数（reads / progs / erases），写性能花在哪儿看它
- **栈高水位**：`exec` 在程序跑起来之前把整块栈填成 `0xCC`，`ps` 的 `stk/sz` 列
  从栈底往上找第一个被改过的字节，就是它至今最深用到的栈（分母是它现在的栈多大）；
  `/proc/stack` 记的是历史最深的那一次（含已经退出的程序）。真机实测 shell 0.6 KiB、
  `spin` 0.2、`edit` 0.6 —— 8 KiB 的默认栈按实测有十几倍余量
- **控制台就是下载线**：`tools/tosput`（Windows GUI / CLI）把宿主文件经串口
  传进 littlefs，板上 `bin/rx` 接收，停等 ACK + 板端 CRC32 校验，零内核改动；
  详见 `docs/FILE-TRANSFER.md`
- **爆栈不是死刑**：栈和堆在窗口里共用一块区，程序踩出栈底时内核看堆还剩多少，
  有 8 KiB 空档就把栈翻倍（8→16 KiB）、把堆的尾巴让给它、重试那条指令，打一行
  `[pid N] stack grown to 16 KiB`；堆先吃满就照旧 fault 杀任务。PMP 仍 4 条规则
  （NAPOT 只能描述 2 的幂，所以是翻倍而不是任意长）。`deep 6` 是这个机制的自测；
  输不起的程序继续用 `TOS_STACK(n)` 显式声明，那条 note 会被 `exec` 读到并决定
  窗口大小
- `stk`：`stk 4 3000 &` 递归四层、每层 2 KiB 局部存储然后睡住，专门用来验证
  上面那个测量本身是对的（`ps` 里必须读到 8.x KiB）
- 输出同时走 VGA 文本模式和 COM1，可用 `-headless` 直接在终端里玩
- 其余：PIT 定时器、PS/2 键盘、PIC 重映射、串口
- **RV32 移植**（`-target rv32`）：M 模式起步、U-mode + `ecall`、CLINT
  定时器、16550 串口、内嵌 littlefs 的 RAM 盘；同一批用户程序原样运行
- **RV32 的 PMP 分段**：每个任务只有 code(RX) / data+heap(RW) / stack(RW) 三段可达，
  堆与栈之间留一段无人认领的空洞；越界写当场 `store access fault`，只杀当前任务
  （x86 关分页后没有这个能力，只有 ring 3 的特权级隔离）

## 目录

```
boot/stage1.S        512 字节引导扇区
kernel/              架构无关内核（C）
  main.c             启动、ramdisk 自检、cpu_init、拉起 /bin/sh
  platform.h         HAL：各 arch/board 实现的平台接口（含时钟、CPU 拓扑、GPIO/ADC）
  cpu.c cpu.h        per-CPU 状态、二级 hart 起步、自旋锁
  periph.c periph.h  GPIO/ADC 的 weak 默认实现与分发，以及 /dev/gpioN 设备注册
  devfs.c devfs.h    设备文件表（boot 时注册、之后只读，open 时无需加锁）
  procfs.c procfs.h  /proc/{ps,mem,uptime}：open 时生成快照文本
  process.c syscall.c  进程/exec/系统调用/管道与描述符
  pipe.c pipe.h      环形缓冲 IPC
  elf.c               ELF32 加载器（PT_LOAD + REL/RELA 重定位，目标窗口校验）
  ramdisk.c           littlefs 挂载与文件访问（块设备回调接 ATA）
  heap.c lz4.c libgcc.c console.c string.c
  lfs_config.h        littlefs 的裁剪与配置
arch/x86/            x86 实现：entry.S isr.S gdt idt ata serial timer keyboard platform.c
  regs.h usys.h      i386 的 trap 帧布局 / 用户态系统调用入口（int 0x80）
arch/rv32/           RV32 实现：start.S（入口 + trap 向量，含第二 hart）
  platform.c         UART/CLINT/PMP/RAM 盘 + 自旋锁（amoswap）
  regs.h usys.h      RV32 的 trap 帧布局（144 B）/ ecall 入口
board/pc/            x86 板级内存布局（layout.c，含 littlefs 分区 LBA）
board/qemu-virt/     QEMU virt (rv32) 板级布局（layout.c）；rv32 与 rv32smp 共用
docs/PLATFORM-DESIGN.md  无 MMU 平坦模型的设计与路线图
docs/SHELL-GUIDE.md   shell 脚本与优先级的详细使用说明（示例均为实跑输出）
docs/OPEN-ITEMS.md   已评估、还没拍板的事项（决策队列，含实测数字）
third_party/littlefs/  上游 littlefs v2.9.3（lfs.c/lfs.h/lfs_util.*）
tools/mklfs.c        host 端 littlefs 打包/校验工具
tools/lz4.c          host 端 LZ4 压缩器（只用于打包）
user/                用户态程序（freestanding C）
  crt0_x86.S crt0_rv32.S  按目标选择的启动桩
  lib.c syscall.c         与架构无关；系统调用入口来自 arch/<arch>/usys.h
  sh.c hello.c hello_printf.c echo.c ls.c cat.c crash.c mem.c uptime.c wfile.c reloc.c
  gpio.c adc.c reboot.c smp.c wc.c
  user.ld            PIE 链接脚本（基址 0，由内核重定位）
cmd/tos/             Go 构建工具（build / run / doctor / clean）
cmd/mkmini/          [未采用] TOSX 容器工具（见 internal/build/minielf.go 头注释）
internal/build/      编译、镜像、QEMU（minielf.go 同为未采用实验）
files/               会被打包进 littlefs 分区的普通文件
include/             freestanding 的 stdint/stdbool/string/inttypes 头
```

> **格式决策**：用户程序保持**标准 ELF32 ET_DYN + 重定位表 + LZ4TOS 信封**。
> 曾评估过的 MiniELF（TOSX，28 字节头）因丢掉 ELF 结构与重定位、无法跨板分发二进制
> 而**未采用**，相关代码（`internal/build/minielf.go`、`cmd/mkmini`）只作参考保留。

## 环境准备

需要三样东西：**Go**、一个能生成 i386 ELF 的 C 工具链、**QEMU**。
Windows 上推荐**只用 MSYS2（UCRT64）**：clang + lld + QEMU 一条命令装齐，
不需要另外下载 Zig 或 QEMU 的安装包。

### 1. Go

安装 Go 1.21+（<https://go.dev/dl/>），确认：

```powershell
go version
```

### 2. MSYS2 clang/lld + QEMU

从 <https://www.msys2.org/> 安装 MSYS2 后，在 UCRT64 终端里执行：

```bash
pacman -Syu
pacman -S --needed mingw-w64-ucrt-x86_64-clang \
                  mingw-w64-ucrt-x86_64-lld \
                  mingw-w64-ucrt-x86_64-qemu
```

`tos` 默认就会在 `C:\msys64\ucrt64\bin` 里找工具，**不需要设置任何环境变量**。
（若安装到别处，用 `$env:TOS_BIN = "D:\msys2\ucrt64\bin"` 指过去。）

> 为什么不是普通的 gcc？MSYS2 的 `gcc` 产出的是 **Windows PE/COFF** 目标文件，
> 不能链接成内核需要的 i386 ELF。内核和用户程序都需要 `i386-elf` 目标。
> 装载器会通过 clang 驱动 lld（`-fuse-ld=lld`）来生成 ELF32；
> 如果装了 `i686-elf-gcc` 交叉工具链，`tos` 也会自动识别。

**Zig 仍然受支持**（自带 clang/lld，单个压缩包），`tos` 会优先使用它。
但它只作为可选方案，不是必须的。

### 3. QEMU

用上面的 `mingw-w64-ucrt-x86_64-qemu` 即可，可执行文件在
`C:\msys64\ucrt64\bin\qemu-system-i386.exe`，`tos` 会自动找到。
如果装在别处，用环境变量指过去：

```powershell
$env:TOS_QEMU = "D:\qemu\qemu-system-i386.exe"
```

> 真机 esp32c3 的烧录/验收脚本 `scripts/esp-c3/` 仍然用 Python（esptool + pyserial
> 是 ESP 生态标准，芯片只能通过它的下载模式协议写入），只在接板子时才需要。
> 构建、模拟器测试和 CI 全部用 Go，不需要 Python。

### 4. RV32EMU-C（RV32 标准测试运行时）

RV32 的功能验收和性能测试默认跑在 **RV32EMU-C** 上，源码以 submodule 形式放在
`rv32emu-c/`（<https://github.com/yafengabc/RV32EMU-C>），不是 `third_party`。
clone 之后先取子模块并构建：

```powershell
git submodule update --init
go run ./cmd/tostest emu build     # -> build/rv32emu-c.exe
go run ./cmd/tostest emu test      # 启动 esp32c3 内核 + uptime/ls 冒烟
```

装了 make 的话也可以 `make emu` / `make test-emu`；否则直接用上面的 Go 入口。
`tostest bench`、`tostest emutest` 也会自动找 `build/rv32emu-c.exe`，
`TOS_EMU` 可覆盖：

```powershell
$env:TOS_EMU = "C:\path\to\rv32emu.exe"    # 例如用 Go 版做差分测试
```

> **时钟**：模拟器早先按「一条指令一个 TIMG 计数」驱动定时器，而且完全忽略
> `T0CONFIG` 的 13:28 分频字段——TinyOS 按读到的 APB 写入除数，模拟器却当成 1，
> 于是 tick 密了 20 倍；时间片恰好是一个 tick，用户程序在两次切换之间连一次
> syscall 都走不完。症状是内核打印到 `starting /bin/sh ...` 就停，`exceptions=0`
> ——从未发出第一个 `ecall`。现在默认走主机墙钟、分频字段生效、三个时基各按
> 自己的频率计数，默认参数即可启动，`tick self-check` 显示 1。
>
> 时钟树本身也建模了：`SYSTEM_SYSCLK_CONF` / `SYSTEM_CPU_PER_CONF` 复位为真机
> 读数（XTAL/2，20 MHz），BBPLL 的 I2C 寄存器文件应答锁定位，所以内核的
> `plat_clock_init()` 能走完整个 PLL 启动序列，打印 `[clk] CPU 160 MHz, APB 80 MHz`
> ——与真机一致。TIMG 的计数率因此从时钟树推导，而不是写死 20 MHz：APB 变了，
> 模拟器与固件仍然对同一个数。`-clk insn` 保留按指令计时的可复现模式；`tostest`
> 的三个入口仍显式带 `-clk wall`，对旧版模拟器二进制同样成立。指令语义一直
> 没问题：`rv32emu-bench.exe check` 四个负载与 QEMU + 独立参考模型全部一致。

为什么 QEMU 只做功能验证、不做性能：QEMU 的 TCG 在 store 落到已翻译的代码页时
会作废整个 TB，`memcpy` 每次迭代都要重新翻译同一段循环——实测同 5 个 TB 被翻译了
8000 多次，所以 `bench` 在 QEMU 上慢两三个数量级（`rv32emu-c` 的 `bench` Go 工具
`check` 里可以复现）。`tos run -target rv32` 继续用于启动/兼容性检查，速度与回归以
RV32EMU-C 为准；细节见 `docs/PLATFORM-DESIGN.md`。

### 5. 检查

```powershell
go run ./cmd/tos doctor
```

应能看到工具链和 qemu-system-i386 的路径与版本。

## 使用

```powershell
# x86：编译内核 + 用户程序 + littlefs 分区，生成 build/tinyos.img
go run ./cmd/tos build

# 在 QEMU 窗口里启动（VGA 文本 + PS/2 键盘）
go run ./cmd/tos run

# 无窗口，直接把终端当串口控制台（推荐；敲键盘完全没问题，粘贴见下方注意）
go run ./cmd/tos run -headless

# 调试：把中断/CPU 复位记录到 build/qemu.log
go run ./cmd/tos run -headless -debug

# RV32：编译出 build/kernel-rv32.elf，并在 qemu-system-riscv32 里启动
go run ./cmd/tos build -target rv32
go run ./cmd/tos run   -target rv32

# RV32 双核：同样的板子，两个 hart（-smp 2，rv32imac）
go run ./cmd/tos build -target rv32smp
go run ./cmd/tos run   -target rv32smp

# ARM：编译出 build/kernel-arm.elf，并在 qemu-system-arm（mps2-an385）里启动
go run ./cmd/tos build -target arm
go run ./cmd/tos run   -target arm
```

进去以后：

```
tos> ls
tos> cat readme.txt
tos> hello one two three
tos> mem
tos> crash          # 故意执行非法指令，只会杀掉 crash 自己
tos> uptime
tos> ps             # 看看当前有哪些任务、各自状态
tos> run echo hi
tos> run spin &     # 后台跑一个死循环程序，shell 仍然可用
tos> nice 2 bench & # 以 2 级（最低）启动：shell 和前台永远先跑
tos> prio 2 1       # 把 pid 2 的任务调到 1 级
tos> run prot heap  # 故意写越界的堆：RV32 上当场被 PMP 拦下，x86 上什么也不会发生
tos> gpio 4 1       # 拉高 GPIO4 并回读（C3 / RP2040 上是真引脚）
tos> adc 0          # 采一次 ADC：raw 计数 + 换算出的毫伏
tos> cat /proc/ps   # 进程表也是文件；`ps > f.txt` 把它写进文件
tos> cat /proc/mem  # 内存地图（和 free 同一套数字）
tos> cat /proc/lfs  # 块设备调用计数：读/写/擦除各多少次
tos> cat /proc/stack # 栈高水位：历史最深的一次，以及是哪个程序用掉的
tos> stk 4 3000 &    # 递归四层各 2 KiB 再睡住，验证栈测量本身
tos> ps              # stk 列 = 每个任务至今最深用到的栈
tos> dd of=/tmp.bin bs=4096 count=80   # 往扩容区写 320 KiB 图案
tos> dd if=/tmp.bin bs=4096 count=80   # 逐字节读回校验
tos> rm /tmp.bin    # 删掉；目录和打开中的文件会被拒绝
tos> ls /dev        # 目录是算出来的：c gpio0 ... c adc4（pc/virt 上是空目录）
tos> echo 1 > /dev/gpio4   # 写设备文件就是驱动引脚（先灌成输出）
tos> cat /dev/adc2         # 读设备文件采样（RP2040 有 5 路，C3 没有）
tos> gpio dev mode 4 out   # 配置面走 ioctl：open + ioctl(GPIO_OP_MODE)
tos> cat readme.txt | wc   # 管道
tos> smp            # 每个 CPU 一个 worker，看它们各自落在哪个核上
tos> source etc/rc  # 在本进程里读一个命令文件（不占新窗口，C3 上最省）
tos> run sh etc/rc  # 也可以把它当成一个程序跑：新任务、新 pid、崩溃不影响 shell
tos> reboot         # 真复位芯片（C3 上 ROM 会报 rst:0x3 RTC_SW_SYS_RST）
tos> exit
```

也可以先编译成 exe，之后直接运行：

```powershell
go build -o tos.exe ./cmd/tos
.\tos.exe build
.\tos.exe run
```

> Windows 上 QEMU 的 `-serial stdio` 输入端在**整行粘贴**时可能丢字节（QEMU 自己的
> 后端限制，不是内核的问题）；一个字符一个字符地敲是没问题的。
> 自动化测试用 `tostest smoke`，它通过 TCP 串口喂命令：

```powershell
go run ./cmd/tostest smoke
```

脚本会启动 QEMU、依次执行 `ls / cat / hello / reloc / run spin & / ps / wfile / mem /
run prot heap / crash / uptime / exit`，检查提示符和内核 panic，最后打印完整记录。
`ps` 那一步等于一次调度器验收：`spin` 必须还活着并处于 `rdy`。
RV32 用同一个命令加 `-target rv32`，ARM 用 `-target arm`：

```powershell
go run ./cmd/tostest smoke -target rv32
go run ./cmd/tostest smoke -target arm
go run ./cmd/tostest smoke -target rv32smp   # 双核：同一批命令，两个 hart 上跑
```

## RV32 目标（qemu-virt）

`-target rv32` 会构建一套 **RISC-V 32 位（rv32imc / ilp32 软浮点）** 的完整系统：
内核、用户程序、littlefs 镜像，全部用同一份源码，不需要第二套实现。

```powershell
go run ./cmd/tos build -target rv32   # -> build/kernel-rv32.elf (+ build/ramdisk-rv32.bin)
go run ./cmd/tos run   -target rv32
```

QEMU 启动命令等价于：

```
qemu-system-riscv32 -machine virt -m 256M -bios none -kernel build/kernel-rv32.elf
```

`-bios none` 表示不用 OpenSBI：内核自己从 M 模式起步，配置自己的 trap 向量、
16550 串口、CLINT 定时器和 PMP，然后 `mret` 到 U-mode 运行 `/bin/sh`。

已经跑通的东西（与 x86 完全相同的一批用户程序）：

- `ls / cat / echo / hello / hello_printf / reloc / mem / crash / uptime / wfile / sh`
- 参数传递（`hello alpha beta` → `argc=3`）、`exec` 嵌套、`sbrk` 堆
- PIE 重定位：`reloc` 里的函数指针表和字符串表在 RV32 上同样被正确修正
- 文件系统真实可写：`wfile demo.txt hello` 之后 `cat demo.txt` 能读回
  （写进内核镜像里那块 RAM 盘，QEMU 退出即丢失；真机换成 SPI flash 即可持久化）
- 用户程序崩溃只杀自己：`crash` 打出 `[pid 10] illegal instruction at 0x80500054`
  后 shell 继续活着
- `exit` 通过 SiFive test 设备真正关机（QEMU 进程退出，不是空转）

实现上值得记一笔的几处（详见 `docs/PLATFORM-DESIGN.md` §8）：

| 主题 | 做法 |
| --- | --- |
| trap 帧 | 144 字节：x0–x31（偏移 = 4×寄存器号）+ `mepc/mstatus/mcause/mtval` |
| 栈切换 | 入口读 `mstatus.MPP`：从 U 模式陷入才换到 32 KiB 内核栈，M 模式原地压栈（可嵌套） |
| 系统调用 | `a7` = 号，`a0/a1/a2` = 参数；内核在派发**前** `mepc += 4`，与 i386 iret 语义一致 |
| PMP | 每任务 4 条：空条目（下界）+ code RX + data/heap RW + stack NAPOT RW，切换时重写；条目 0 只能从地址 0 起，所以下界靠“上一条的 `pmpaddr` 即使 OFF 也生效”来表达 |
| 用户态入口 | `rv32_enter_user()` 复用 trap 返回路径，`mret` 进 U-mode |
| 缺的运行时 | `kernel/libgcc.c` 补 `__lshrdi3/__ashldi3/__ashrdi3`（RV32 无 64 位移位指令） |
| 块设备 | `build/ramdisk-rv32.bin` 由生成的 `.S` 用 `.incbin` 嵌进 `.data.ramdisk`，`plat_block_*` 直接内存拷贝 |

工具链：`clang --target=riscv32-unknown-elf` 编译，`ld.lld -m elf32lriscv` 直接链接
（走 clang 驱动会强行拉入硬浮点 ABI 的 `compiler_rt`，和我们的 ilp32 对象冲突）。
需要 `qemu-system-riscv32`：MSYS2 的 `mingw-w64-ucrt-x86_64-qemu` 已经带上。

## 双核（SMP）目标（qemu-virt，两 hart）

`-target rv32smp` 是同一块 qemu-virt 板子的**两核版本**：`-march=rv32imac`
（比单核多一个 A 扩展，自旋锁就是一条 `amoswap.w.aqrl`）、`-DTOS_MAX_CPUS=2`、
QEMU 侧加 `-smp 2`。

```powershell
go run ./cmd/tos build -target rv32smp   # -> build/kernel-rv32smp.elf
go run ./cmd/tos run   -target rv32smp

# 等价的手工启动
qemu-system-riscv32 -machine virt -m 256M -bios none -smp 2 \
    -kernel build/kernel-rv32smp.elf

# 自动化：同一批冒烟命令，在两个 hart 上跑
go run ./cmd/tostest smoke -target rv32smp
```

启动时多打一行 `[smp] 2 of 2 CPU(s) online`。看效果：

```
tos> smp
smp: 2 CPU(s), tick 1000 Hz, this task on cpu 0
smp: starting 2 worker(s), 800 ms each
worker 0: cpu 0, 167 blocks in 800 ms
worker 1: cpu 1, 222 blocks in 800 ms
smp: done
tos> run smp &
tos> ps
 pid ppid prio state  c cpu% cpums wink stk  name
   1    0    1 run   0    0.7     0   32   0.6  sh
   5    1    1 wait  -    0.0     0   32   0.5  smp
   6    5    1 rdy   -   93.7     0   32   0.4  smp
   7    5    1 run   1   86.5     0   32   0.4  smp
```

两个 worker 各占一个 hart（`c` 列是正在跑它的 CPU），各自接近 100% —— 是真并行，
不是轮流。`smp` 自己还会报 `cpu 1 then 0`，那是任务在两个核之间迁移过。

设计上做了这些事（详见 `docs/PLATFORM-DESIGN.md` §13）：

| 主题 | 做法 |
| --- | --- |
| 每 hart 的栈 | `start.S` 的 `.bss` 里 `32768 * TOS_MAX_CPUS`，hart N 用 `__kernel_stack_top - N*32K`，`CPU_KSTACK_TOP(id)` 是唯一算式，栈溢出守卫每块一个 |
| 每 hart 的 trap | `mtvec` 是 per-hart CSR，所以有两张 256 字节向量表（`trap_vectors` / `trap_vectors_h1`）；`TRAP_ENTRY` 宏参数化成 `trap_entry_h0/h1`，各自 `la` 自己的栈顶 |
| 从 hart 起步 | 所有 hart 都从 `_start` 进；`csrr mhartid` 分流，非 0 的等 `cpu_release == "SMP1"` 再去碰任何内核数据（否则会和清 `.bss` 的 0 号撞车） |
| 每 hart 的状态 | `cpu_t`（`kernel/cpu.h`）：`cur / quantum / need_resched / rr`。`process.c` 用同名的三个宏取，所以单核代码一行没改 |
| 共享的部分 | 只有进程表。一把 `spin_t proc_lock`，锁住的范围是“扫一遍表 + 改状态”，不含任何会阻塞的调用 |
| 定时器 | `mtimecmp` and `mie.MTIE` 都是 per-hart：`plat_timer_start_cpu()` 给新 hart 装自己的比较值。`g_ticks` 只由 0 号递增，否则时钟会走两倍 |
| 控制台 | 只有 0 号 hart 读串口 FIFO、发回显（一个设备一个驱动）；输出流一把 `out_lock`，一次 `printf`/`write` 不会被另一个核切开 |
| 调度 | 共用一条运行队列，两个 hart 各自挑。空闲的 hart 发现“没有 ready”时会再看一眼别的 hart 是否在跑，否则会误判死机 |

## ARM 目标（QEMU mps2-an385，Cortex-M3）

`-target arm` 构建 **Thumb-2 / ARMv7-M（Cortex-M3，软浮点）** 的同一套系统：
内核、用户程序、littlefs 镜像，用户 ELF 仍是标准 ELF32 ET_DYN + LZ4 信封。

```powershell
go run ./cmd/tos build -target arm   # -> build/kernel-arm.elf
go run ./cmd/tos run   -target arm
```

等价的 QEMU 启动命令：

```
qemu-system-arm -machine mps2-an385 -kernel build/kernel-arm.elf
```

mps2-an385 上，镜像加载地址就是 `0x00000000`，向量表的头两个字号是初始 MSP 和复位
向量（所以 `.vectors` 必须排在 `kernel.ld` 的最前面）；`.data/.bss`、内核栈和用户窗口
都在 `0x20000000` 起的 4 MiB SRAM 里。

同样跑通的一批程序：`ls / cat / hello / hello_printf / reloc / mem / crash / uptime /
wfile / sh`，参数传递、`exec`、`sbrk`、后台 `spin &` 抢占、`exit` 关机都与其它目标一致。

| 主题 | 做法 |
| --- | --- |
| 特权级 | Thread 模式 + PSP 跑任务（**非特权**，`CONTROL.nPRIV=1`），Handler 模式 + MSP 跑内核，`svc #0` 进内核 |
| 内存保护 | MPU 8 个区域：窗口 RW+XN、`[win_base, wx)` 按 32 B 起的 2 的幂块 RO+可执行、栈下 16 KiB 护栏；内核走 `PRIVDEFENA` 默认映射 |
| trap 帧 | `regs_t` 20 个字号 = 80 B：r4–r11 + `exc/vec/hw/kern_sp` + 硬件帧副本（`exc/vec/...` 与 `start.S` 的 `FR_*` 共享，`regs.h` 有编译期尺寸校验） |
| 首次进用户态 | `arm_enter_user()` 发布帧后主动 `svc`，由 `arm_svc` 的 pending 分支换成任务帧（EXC_RETURN 只在异常返回路径上有效） |
| 栈切换 | `arm_resume` 按 EXC_RETURN 的 S 位决定是否重设 PSP，并恢复 MSP/r4–r11 |
| 定时器 | SysTick @25 MHz 参考时钟，1000 Hz；**优先级必须高于 SVCall**，否则阻塞在 syscall 里的 `wfi` 永远等不到 tick |
| 块设备 | 与 rv32 相同的 `.data.ramdisk`（`.incbin` 嵌入），`plat_block_*` 直接内存拷贝 |
| 缺的运行时 | `kernel/libgcc.c` 补 `__aeabi_memcpy4/8`、`__aeabi_memset/memmove`，以及按 EABI 返回 `r0:r1`/`r2:r3` 的 `__aeabi_*divmod` |

**MPU + 非特权用户态已实现**：每个任务有 8 个 MPU 区域——区域 0 是整个窗口
（RW、不可执行），区域 1–6 把 `[win_base, wx)` 按 2 的幂块标成只读可执行（32 B
粒度，`user.ld` 已把可写段对齐到 32 B），区域 7 是栈底 16 KiB 的护栏。内核在
Handler 模式靠 `PRIVDEFENA` 访问整片 RAM/MMIO，非特权访问默认映射一律拒绝。
`nPRIV` 在 `arm_resume` 里、`bx lr` 之前写（Handler 模式忽略它，所以中断仍是特权）。

能力边界（ARMv7-M 的 MPU 没有 TOR，只能写自然对齐的 2 的幂块）：

- `run prot null` / `prot rodata` / `prot stack` 都会当场 MemManage，只杀当前任务；
- `run prot heap` 会 “survived”——`brk` 只能对齐到区域边界，写越界一字节拦不住；
- 栈溢出 ≤ 16 KiB 被护栏拦下，更深则会踩进窗口里。

细节和踩坑记录见 `docs/PLATFORM-DESIGN.md` §5.2、§8.0。

## 加一个自己的程序

在 `user/` 下新建 `foo.c`：

```c
#include "lib.h"

int main(int argc, char *argv[])
{
    printf("hi from foo, %d args\n", argc);
    return 0;
}
```

然后 `tos build`。构建工具会自动编译 `user/*.c` 里除 `lib.c`/`syscall.c` 外的每个文件，
链接成 ELF 并放进 littlefs 镜像的 `bin/foo`，shell 里直接输入 `foo` 就能运行。

程序大到写不下一个文件时，把它拆成 `user/<name>/` 目录：目录里的每个 `.c` 是一部分，
整个目录链接成**一个**程序 `bin/<name>`。这样解释器那样的几千行代码是一个任务，
而不是几千个任务。目录里还可以放一个 `opt` 文件，写一个编译优化级别给这个程序
单独用（`-Oz` 会让 RV32 代码在 QEMU 的 TCG 下慢约 100 倍，所以只有需要压体积的
程序才这么写，内核不用）。

普通数据文件放到 `files/` 下（可以带子目录），会以同样的相对路径出现在 littlefs 里，
用 `cat` 读取。

## 脚本

命令文件由同一个 `run_command()` 解释，但比控制台多三样东西：**变量**、
**用管道取回程序输出**、**成块的控制语句**。控制台也认变量和 `$(...)`
（方便当场调试），只有 `if` / `while` / `done` 这类需要跳回开头的语句必须写在
文件里：

```sh
# files/etc/demo.sh（节选；随镜像打包，也是 tostest emutest 的回归夹具）
name=$(echo tiny)          # echo / pwd 直接给文本；其他名字走 bin/<prog>
t=$(cat /proc/uptime)      # fd 1 接 pipe → spawn → 读到 EOF → wait
if $t != 0
    echo [script] capture-ok
fi
hello                      # 调用程序
echo abc def | wc          # 两个程序之间的管道
i=0
while $i -lt 3
    echo [script] loop $i
    incr i
    sleep 10               # 脚本内的内建，不占任务窗口
done
echo [script] done
```

`$(...)` 里只放一条简单命令（内部可以有 `$var`，不支持 `|` / `>` / 嵌套）；
结果里的换行变空格、尾部空白去掉，超过 255 B 会明说截断。条件只有一次比较：
`==` / `!=` / `-eq` / `-ne` / `-lt` / `-gt` / `-le` / `-ge` / `-n a` / `-z a`，
`break` 可以从 `if` 里跳出最内层 `while`，`else` 可省。变量 8 个（值 95 B），
`$?` 是上一条命令的退出码；`incr` 是唯一的算术。

**`sleep` / `echo` 只在脚本里内建**：每毫秒采样一次不能为一次 `sleep 1` 付一次
exec + ELF 重定位。但内建会停住 shell 自己，而内核拒绝向 init 投递信号，
`source` 脚本里的长 sleep 因此收不到 `^C`。所以控制台上的 `sleep 3000`、
`sleep 500 &`、`nice 2 sleep 500`、`run sleep 500` 仍走 `bin/sleep` 程序；
脚本里需要可中断的长等待也请写 `run sleep N`。

两种跑法，区别只在"要不要多一个任务"：

| 写法 | 做什么 | 任务窗口 | 适合 |
| --- | --- | --- | --- |
| `source <file>` | 在当前 shell 进程里递归读文件，不 `exec` | 不占窗口 | 嵌套脚本；C3 上跑任何东西 |
| `run sh <file>` | `exec` 一个新的 sh，文件名作参数 | 一个 32 KiB 窗口 | 崩溃隔离、`&`、要退出码 |

`source` 仍然值得推荐，因为它连窗口都不占：C3 的 192 KiB 用户区按需分配，一个新
sh 就是 32 KiB，嵌套几层就把并发空间吃掉了。嵌套深度上限 `MAX_SOURCE = 4`；
脚本整份读进 **4 KiB 静态池**（`source` 追加、返回时退回，所以循环里反复 source
不会累积），每个脚本最多 64 行、单行 256 B，块嵌套 8 层，`while` 最多回跳
10000 次——脚本跑在 shell 自己身上，没有独立任务可供 `^C` 收拾，死循环得兜住。
`exit N` 结束脚本并返回 N（在控制台才是关机），`source` 的返回值进 `$?`。

启动时 pid 1 会读一次 `etc/rc`（源文件 `files/etc/rc`，随镜像打包）。它默认是空的：
开机自己说话的机器没法做差分测试，而这里打印的每个字节都会落在所有验收用例前面。

完整语法、限制表和实测输出见 `docs/SHELL-GUIDE.md` §1；体积与内存的取舍另见
`docs/SCRIPT-DESIGN.md`。

## Python 子集解释器

`bin/py` 是一个**用户程序**（不是内核功能）：它有自己的任务窗口、自己的堆、自己
的栈，和 `sh` 一样是 `exec` 出来的普通任务，`^C` 也能打断它。

```sh
py demo.py            # 跑一个文件
py -c print(1 + 2)    # 跑一行
py -v demo.py         # 顺便打印 arena 用了多少
```

实现的语言是 Python 的一个子集，够写小程序，不够写整个 Python：

| 有 | 没有 |
| --- | --- |
| 缩进分块（`if`/`elif`/`else`、`while`、`for`、`def`） | `try`/`except`、`with`、`class`、装饰器 |
| `int`（32 位）、`str`、`list`、`dict`、`bool`、`None` | 浮点（三个目标都没有 FPU）、元组、`set`、字节串 |
| `+ - * / %`、比较、`and`/`or`/`not`、`in` | `**`、位运算、`//`、切片的步长 |
| 赋值、增强赋值（`+=` 等）、`del`、`break`/`continue`/`return` | `global`/`nonlocal`（作用域规则见下）、`lambda` |
| 函数（位置参数 + 关键字参数）、递归、闭包 | 生成器、异步、解包赋值 |
| 内建：`print len range str int type bool abs sum min max list dict` | `open`、`input`、`zip`、`map`、`sorted` |
| 方法：`list.append/pop/insert/remove/index/clear`，`dict.get/keys/values/items/pop`，`str.upper/lower/strip/split/join/replace/find` | `list.sort`、`dict.setdefault`、字符串格式化 |
| 异常按 Python 的样子报（`NameError: name 'x' is not defined` + 出错的行） | 异常对象、`try` |

几个刻意偏离 Python 的地方：

- **`/` 是整数除法**。三个目标都没有浮点硬件，64 位除法的辅助函数链接不上，
  所以整数一律 32 位，`/` 向零截断（`7 / 2 == 3`）。
- **没有 `global`**。Python 里函数体赋值的名字一律是局部的，要改全局得写
  `global`。这里改成：**名字已经绑定在哪里就改哪里**，否则在本层新建。
  于是计数器那种写法不需要 `global`。
- **`dict` 按插入顺序**（和 Python 3.7 一样）。它内部是一个按插入序排列的数组，
  不是哈希表：几十个键的线性查找就是一次缓存行内的查找，顺序比复杂度值钱。
- **循环引用不回收**。对象是引用计数的，`a = []; a.append(a)` 会泄漏。
  这是有意的取舍：循环收集器需要根集合和标记阶段，而脚本进程退出时内核会把整个
  窗口收回去。

内存是这件事的主要约束。解释器自带一个 arena（**不是** TLSF）：语法树、token
列表、名字文本和脚本建出来的对象都住在里面。arena 是任务堆的一次 `sbrk`，
大小由 `TOS_HEAP(96 * 1024)` 声明 —— C3 上那是一个 144 KiB 的窗口，
192 KiB 的用户区里剩下的正好是 shell 的 32 KiB。

实测（ESP32-C3，`rv32emu-c`）：

| 脚本 | arena 物理占用 |
| --- | --- |
| `demo.py`（100 行，算术/字符串/列表/字典/循环/函数/递归） | 83 KiB |
| `pytest.py` + `pytest2.py`（75 条断言） | 各约 70 KiB |

arena 里最贵的是语法树和 token 列表：它们永不释放，所以用**无块头的永久分配**
（`py_alloc_perm`），省掉每个块 16 字节的块头和按 16 字节取整的浪费 —— 树节点
48 字节就真的只占 48 字节。

`files/demo.py` 是功能演示，`files/pytest.py` 和 `files/pytest2.py` 是断言套件
（`py pytest.py` 的输出每行一条，最后一行是 `checks: N failed: 0`）。
两者都随镜像打包，也是 `tostest emutest` 的回归项。

## 进程与调度

`exec` 只负责**创建**子任务并返回它的 pid，父任务继续跑；要退出码就 `wait(pid)`。
shell 的 `run` 就是 `exec` + `wait`，所以前台表现和你熟悉的一样：

```
tos> run hello deep      # 前台：shell 阻塞在 wait 里，直到 hello 退出
tos> run spin &          # 后台：立刻回到提示符，[pid] 是子任务
```

调度是**三级优先级（0 最高）+ 级内时间片轮转**，1 ms 一片（1000 Hz tick），由定时器
tick 驱动。唤醒也抢占：级别更高的任务被叫醒（输入行到达、sleep 到点、子任务退出）时
立刻标记重新调度，切换仍在 trap 收尾完成。**只在用户态帧上抢占**：
trap 落进内核态时（比如 syscall 执行中），那次内核调用一定跑完——内核栈上压着
它的调用帧，中途换走就再也接不回来了。

级别是继承的，且默认只有 `NORMAL(1)`：`nice <0-2> <prog>` 以指定级别启动一个程序，
`prio <pid> <0-2>` 移动一个在跑的任务。**只有 shell 能往 0 级放东西**（`nice 0`），
普通程序既不能把自己也不能把子任务抬到 shell 之上——0 级是留给实时任务的，
"有它就轮不到别人"；其余一律只能往下调。级间是严格的——高一级只要有一个任务
可运行，低一级一个 tick 都拿不到；没有 aging，饿死是刻意的行为。验证方法：

```
tos> nice 2 spin &       # 2 级（LOW）
[2]
tos> nice 0 spin &       # 0 级（实时）
[3]
tos> ps
 pid ppid prio state  c cpu% cpums wink stk  name
   1    0    1 run    -    0.0     0   32   0.6  sh
   2    1    2 rdy    -    0.1     0   32   0.2  spin
   3    1    0 rdy    -  100.0     0   32   0.2  spin
```

0 级的 `spin[3]` 会先打完自己的 6 条输出，2 级的 `spin[2]` 一条都插不进来；
它一退出，`spin[2]` 立刻补上。把两行都发成 0 级则是标准的 50/50 轮转。

`ps` 显示 pid、ppid、优先级、状态、CPU 占用和程序名：

```
tos> run spin &
[2]
tos> ps
 pid ppid prio state  c cpu% cpums wink stk  name
   1    0    1 run    -    0.0     0   32   0.6  sh
   2    1    1 rdy    -   70.0     0   32   0.2  spin
```

`rdy` 的意思是"可运行、这一瞬间不在 CPU 上"。`spin` 是个只烧 CPU、从不主动让出的
演示程序（`user/spin.c`）：没有抢占，它会霸占 CPU 直到自己结束，shell 完全没反应。

**阻塞而不是忙等**：`read(0)` 没凑满一行、`wait(pid)` 的子任务还没退出、`sleep(ms)`
时间没到，这三种情况任务直接把 CPU 交出去，由定时器 tick 或子任务退出把它叫醒。
所以 `sleep` 不再空转，后台任务也不会饿死前台。

| 状态 | 含义 |
| --- | --- |
| `run` | 正在 CPU 上 |
| `rdy` | 可运行，等轮到自己 |
| `wait` | 等子任务退出 |
| `tty` | 等一整行输入 |
| `sleep` | 睡到某个 tick |

内核只有一个栈，切换就是"让 trap 收尾去恢复另一个任务的寄存器帧"，
细节见 `docs/PLATFORM-DESIGN.md` 第 6 节。

**回显不会插进程序的输出里**。按键回显不是任务干的：它由 tick 里的 `tty_poll()`
产生，而 tick 落在哪里是定时器说了算——包括别人 `write()` 到一半的时候。所以回显
先排队（`console_echo()`），只在"这一 tick 里没有任何任务输出"时才放出去
（`console_echo_tick()`）。`cat` 一块一块地写、后台程序和 shell 同时输出时，
这一点尤其明显：命令的字符不会再从某行中间挤出来。

## 文件系统

文件系统是 [littlefs](https://github.com/littlefs-project/littlefs) v2.9.3，
源码 vendor 在 `third_party/littlefs/`，**住在磁盘上**（ATA 分区的 LBA 181 起，300 KiB）。
构建流程：

1. `tos build` 把要打包的文件暂存到 `build/fs-stage/`
2. 用 Zig 把 `tools/mklfs.c` + `lfs.c` + `lfs_util.c` 编成 host 程序 `build/mklfs.exe`
3. `mklfs.exe` 用 littlefs 自己 `lfs_format` + `lfs_file_write` 生成 `build/ramdisk-<target>.bin`
   （按目标分开命名：每个目标装的是自己的用户程序，共用一个名字会让最后构建的那个覆盖前面所有）
4. 内核启动后用同一份 littlefs 源码 + ATA 驱动 `lfs_mount` 磁盘上的分区

这样磁盘格式由 littlefs 自己产生和校验，不需要手写格式化器，也不会出现
"构建端和运行端对格式理解不一致"的问题。

内核侧配置（`kernel/lfs_config.h`）：关闭所有日志/断言，`LFS_NO_MALLOC`，
把 `LFS_MALLOC/LFS_FREE` 接到 `kernel/heap.c` 的 `kmalloc/kfree`。文件句柄用
`lfs_file_opencfg()` 指定缓存，但那 512 B 缓存是**在 open 时从内核堆分配、close
时归还**的：空闲句柄只占约 108 B，8 个句柄的 `.bss` 共 864 B（静态缓存方案要 4.9 KiB），
RAM 只为真正打开的文件付。

块设备回调在 `kernel/ramdisk.c`：`bd_read/bd_prog` 用一个 4 KiB bounce 缓冲做
读-改-写（littlefs 可能一次请求整个块），`bd_erase` 把整块写成 0xFF（NOR 语义）。
启动时做一次写自检（创建→读回→删除 `selftest.tmp`），确保 ATA 写路径可用。

### 编译优化等级

默认 `-Oz`（`TOS_OPT` 可覆盖）。本项目以体积为第一优先级，`-Oz` 在这套代码上
每一处都比 `-Os` / `-O2` 小，而 shell 与加载器都是 I/O 受限，启动时间无可测差异：

| | `-O2` | `-Os` | `-Oz`（默认） |
| --- | --- | --- | --- |
| 内核镜像 | 45,272 B | 37,288 B | **35,320 B** |
| 用户程序（14 个） | — | — | **20,756 B** |
| 压缩后分区占用 | — | — | **14,572 B** |

### 指令压缩（RVC）

rv32 目标用 `-march=rv32imc`；`-target rv32nc`（`rv32im`，无 C 扩展）只作度量用，
同一份代码换个编码，冒烟测试同样全绿（2026-11-03 实测）。同口径对比：

| | rv32imc（C） | rv32im（无 C） | RVC 省下 |
| --- | --- | --- | --- |
| 内核 `.text` | 47,018 B | 67,512 B | **20,494 B（30.4%）** |
| 内核代码 LZ4 后（.text+.rodata） | 41,083 B | 47,233 B | **6,150 B（13.0%）** |
| 用户程序 22 个（原始） | 48,948 B | 57,404 B | **8,456 B（14.7%）** |
| 用户程序 22 个（LZ4 -9 后） | 34,516 B | 40,067 B | **5,551 B（13.8%）** |

两个结论：

1. **RVC 本身值 30% 的代码体积**——这是"白拿"的：ISA 原生解码，零启动成本、
   零 RAM 代价，C3/RP2350 这类真实 MCU 都支持。
2. **LZ4 吃掉的是同一份冗余**：无压缩代码 LZ4 能压到 66%，RVC 代码只能压到 81%——
   两者叠加后 RVC 的优势从 30% 缩到 **13% 左右，但仍然稳定为正**。若存储侧真要用
   LZ4（或 LZMA，压缩率更高、对 RVC 的抵消也更强），RVC 依然不亏，只是收益变小；
   而运行时始终是 RVC 直接小 30%。


调度器（P2.5）让内核镜像比上一版大 1,684 B（+5.1%），`.bss` 大 800 B（任务表变宽）：
其中 ~1.3 KB 是 `sched_switch`/阻塞路径/tty 行规程的代码，剩下的主要是 `ps`
的输出格式串和一个后台演示程序。这是本项目唯一一处主动接受体积增长的地方——
没有抢占，"一个死循环程序 = 整机失去响应"。

之后的两项加固又加了 320 B（+0.9%）：回显队列与内核栈守卫，见「调试」一节。

PMP 分段再加 384 B（+1.1%）：`plat_task_protect()`、窗口重排、`elf_probe` 的
w^X 与入口校验。其中约一半是两条新的错误文案（`elf: segment %u is both
writable and executable` / `entry 0x%x is not in an executable segment`），
逻辑本身不到 200 B。这块和调度器不同：它换来的不是"能用"，而是
"用户程序撞墙时只死自己"。

RV32 内核（`.text` 26,564 + `.rodata` 2,500 = 29,064 B 代码/常量，`.bss` 79,504 B
含 32 KiB 内核栈；`.data` 里的 300 KiB 是内嵌的 littlefs 镜像，真机上对应 SPI flash）。

老版本 gcc（< 12）不认 `-Oz`，用 `TOS_OPT=-Os` 回退；调试时用 `TOS_OPT=-Og` 或 `-O0`。

### 程序压缩

`mklfs` 在打包时把**以 `\x7fELF` 开头的文件**用 LZ4 压成信封格式：

| 偏移 | 内容 |
| --- | --- |
| 0 | `"LZ4TOS"`（6 字节魔数） |
| 6 | `u16` flags（必须为 0） |
| 8 | `u32` 原始大小 |
| 12 | `u32` 压缩后大小 |
| 16 | LZ4 块数据 |

内核 `proc_create()` 检测到魔数就 `kmalloc` 到原始大小、`lz4_decompress_block()`
解压，再交给 `elf_load()`；没有魔数的文件（如 `readme.txt`）原样使用。
解压器在 `kernel/lz4.c`，约 420 字节内核代码，带完整边界检查，损坏的块返回错误
而不是越界访问。

实测用户程序（14 个，x86 目标）从 23,628 字节压到 15,691 字节（约 66%），单次
`exec` 多花一次内存分配和 memcpy 级别的时间。

手动检查镜像内容：

```powershell
.\build\mklfs.exe -v .\build\ramdisk-x86.bin
```

## 磁盘布局

| LBA | 内容 |
| --- | --- |
| 0 | 引导扇区（512 字节） |
| 1 – 180 | 内核镜像（最多 90 KiB） |
| 181 – 780 | littlefs 分区（600 扇区 = 300 KiB） |

引导扇区只读内核（`int 13h`），**不再加载文件系统**；内核起来后由 ATA 驱动按需读盘。
LBA 常量由板级提供（`board/pc/layout.c` 的 `PLAT_LFS_LBA = 181`），
`boot/stage1.S` 与 `internal/build/build.go` 的 `KernelSectors` 需保持同步；
构建时对内核镜像大小和 `.bss` 结尾都有检查。

RV32 没有磁盘，构建工具把 `build/ramdisk-rv32.bin` 用生成的 `.S`（`.incbin`）
嵌进内核的 `.data.ramdisk`，板级给 `PLAT_LFS_LBA = 0`，`plat_block_*` 就是内存拷贝。

## 内存布局

物理内存 / 引导期：

| 地址 | 内容 |
| --- | --- |
| `0x00007C00` | 引导扇区 |
| `0x00010000` | 内核镜像（最多 90 KiB）+ `.bss`（约 63 KiB，不占磁盘） |
| `0x0009F000` | 引导期栈 |

镜像里**只存 `.text/.rodata/.data`**，`.bss` 的零字节不落盘，由 `kernel/entry.S`
在 `kmain` 之前清零。内核（含 `.bss`）必须结束在 `0x400000` 之前，
`tos build` 会检查并打印余量：

```
image 35320 / 92160 bytes, footprint ends at 0x286d4 (limit 0x400000, 3934 KiB headroom)
```

没有 ramdisk 了，所以也就不存在实模式 DMA 空洞（`0xA0000`）的限制——文件系统在磁盘上，
内核不再整块拷贝它。

用户地址（平坦模型，全部是物理地址）：

| 地址 | 内容 |
| --- | --- |
| `0x01000000`–`0x08000000` | 任务窗口区，按需分配（步长 `PLAT_APP_GRAIN`，x86 上 64 KiB） |
| 窗口 + 0 | ELF 代码（RX，可读可执行，不可写） |
| 窗口 + 代码长度 向上 | ELF 数据 + `.bss` + `sbrk` 堆（RW），堆上限到栈下 16 KiB |
| 堆顶到栈底之间 16 KiB | **空洞**：不归属任何区域，写越界当场 fault（C3 上这个洞是 4 KiB） |
| 窗口顶部 − 32 KiB 到顶 | 8 页用户栈（RW），栈顶即窗口顶，argv 从顶部往下 256 B 开始 |

没有分页、没有 CR3 切换：`exec` 只是把镜像拷到窗口里再把栈指针指好，退出时把
窗口槽位还回位图。x86 上**没有任何内存保护**——用户程序能读写内核内存，
这是 MCU 级 OS 的刻意取舍。RV32 和 ARM 不是：PMP / MPU 把上面这张表变成了硬件
强制（见下面两节），x86 只能靠约定。

### RV32 的 PMP 分段

`plat_task_protect()` 在每次切换时（以及 `sbrk` 之后）为当前任务写 4 个 PMP 条目：
一条空条目提供窗口下界、`[win_base, wx)` 是 RX、`[wx, brk)` 是 RW、
栈是 32 KiB 的 NAPOT 区间。堆与栈之间的空洞没有条目覆盖，所以：

- 写 .rodata 或代码 → `store access fault`；
- 写越过 `brk`（没 `sbrk` 就要用）→ `store access fault`，`mtval` 是那个地址；
- 栈压穿 → `store access fault`；
- 读写内核、MMIO、窗口外 → fault。

内核只杀这一个任务，窗口槽位收回，shell 照常。M 模式不受 PMP 约束（没置 lock 位），
所以内核仍是那个平坦模型，`memcpy` 用户缓冲区不需要额外机制。
`run prot rodata|heap|stack|null` 就是逐条撞这些墙的演示程序；
在 x86 上它会老实打印 `survived: this machine cannot stop us`。

细节（为什么栈用 NAPOT、为什么空条目能当边界）见 `docs/PLATFORM-DESIGN.md` 5.1。

### ARM 的 MPU 区域

Cortex-M3 只有 8 个 MPU 区域，且每个区域必须是自然对齐的 2 的幂块，没有 PMP 那种
TOR 上界。`plat_task_protect()` 的拼法是：区域 0 覆盖整个 256 KiB 窗口（RW、XN），
区域 1–6 把 `[win_base, wx)` 按 32 B 起的 2 的幂块标成只读可执行，区域 7 是栈底
16 KiB 的护栏（内核可写、用户不可访问）。窗口外的 RAM、MMIO、其它任务都落进
“默认映射”，而 `MPU_CTRL.PRIVDEFENA` 只把它留给特权代码。

因此：

- 写 .rodata 或代码 → `MemManage`（`MMFAR` 给出地址）；
- 读写内核、MMIO、窗口外 → `MemManage`；
- 栈向下压穿 16 KiB 以内 → `MemManage`；
- 写越过 `brk` 一字节 → **不会 fault**：`brk` 不是 2 的幂边界，MPU 表达不了，
  要等 `sbrk()` 把这块算进去。这是与 RV32 唯一的实质性差距。

`nPRIV` 的置位在 `arm_resume` 里、`bx lr` 之前完成（Handler 模式忽略该位，
所以之后到达的中断仍是特权）。细节见 `docs/PLATFORM-DESIGN.md` 5.2、8.0。

## 系统调用 ABI

| | 陷入指令 | 调用号 | 参数 1–3 | 返回值 |
| --- | --- | --- | --- | --- |
| x86 | `int 0x80` | `eax` | `ebx/ecx/edx` | `eax` |
| RV32 | `ecall`（U-mode） | `a7` | `a0/a1/a2` | `a0` |

内核侧不直接读这些寄存器：`plat_frame_get_syscall()` / `plat_frame_set_ret()`
把各架构的约定映射成 `syscall_t{nr, a0, a1, a2}`（见 `arch/*/regs.h`）。
下面的编号与参数顺序两个架构完全一致。

| # | 名称 | 说明 |
| --- | --- | --- |
| 0 | exit | 终止当前进程，退出码放 `ebx` |
| 1 | write | `write(fd, buf, len)`，fd 1/2 = 控制台，fd ≥ 3 写文件 |
| 2 | read | `read(0,...)` 按行读取（带回显，没凑满一行就阻塞），fd ≥ 3 读文件 |
| 3 | exec | `exec(path, argv, argc)` 创建子任务并返回 pid（不等待） |
| 4 | open | 打开 littlefs 文件（只读），返回 fd（≥ 3） |
| 5 | close | 关闭 fd |
| 6 | readdir | `readdir(index, buf, len)`，枚举磁盘上的文件（含 `bin/` 前缀） |
| 7 | getpid | 当前 pid |
| 8 | sleep | 毫秒（阻塞，不忙等） |
| 9 | uptime | 开机毫秒数 |
| 10 | sbrk | 扩展堆，返回旧 brk（0 表示失败） |
| 11 | create | `create(path)` 创建/截断文件并返回可写 fd（≥ 3） |
| 12 | wait | `wait(pid)` 等子任务退出并返回其退出码（阻塞）；`-1` 表示任意子任务 |
| 13 | yield | 主动让出 CPU，返回 0 |
| 14 | ps | 打印任务表，返回任务数 |
| 15 | signal | `signal(pid, sig)`，`SIGKILL` 杀、`SIGSTOP`/`SIGCONT` 停/续（终端 `^C` 走这条） |
| 16 | reboot | 复位机器并从 flash 重新启动，不返回 |

`reboot` 是唯一一个按平台差别很大的调用：QEMU virt / x86 写入各自的复位设备
（SiFive test `0x7777`、键盘控制器 `0xFE`），ARM 端口写 `AIRCR.SYSRESETREQ`，
ESP32-C3 写 `RTC_CNTL_OPTIONS0` 的 `SW_SYS_RST` 位（真机实测复位原因
`rst:0x3 (RTC_SW_SYS_RST)`）。所有端口都先把控制台排空再复位，否则最后一行
会被复位截断；做不到真复位的端口退化成停机，因此调用者永远不会返回。

用户程序可以直接往文件系统写文件了，例如：

```
tos> wfile demo.txt hello from the filesystem
wrote demo.txt
tos> cat demo.txt
hello from the filesystem
```

x86 上写入落在 ATA 分区，**重启后仍然存在**；RV32 上落在内核内嵌的 RAM 盘，
QEMU 退出即丢失（真机换成 SPI flash 驱动即可持久化）。

### 写 3 MiB 到 C3 的 flash 要多久

真机实测约 **60 秒（≈50 KiB/s）**，`cat /proc/lfs` 说明这钱花在哪：

```
  reads         7166 (7173 sectors)
  progs         5937 (3039744 bytes)
  erases         743
```

740 个 4 KiB 数据块 = 743 次 NOR 扇区擦除 + 5,937 次 512 字节编程，**擦除占掉一半
墙钟**。两条优化路都试过，结论写在这里免得再试一次：

- **512 字节写合并缓存**（原计划：把 4 字节的 CTZ 指针更新攒满一个扇区再落盘）
  一个字节都没攒到——littlefs 的 `prog_size` 正好等于设备扇区，所有 prog 都是
  512 对齐的整扇区写，缓存分支从未命中。已删除，`/proc/lfs` 的计数器是留下证据。
- **去掉 `plat_block_write` 的 minimal-change**（原来每个扇区先读回来、逐字比较，
  只写不同的部分）：读次数从每块 9 次降到 1 次量级，3 MiB 写从 ~12,800 次扇区读
  降到 7,166 次，墙钟提升约 13%。这块 flash 的擦除时间就是剩下的物理成本。

## 已知限制（有意保持简单）

- 时间片 1 ms（1000 Hz tick）。三级优先级没有 aging，高一级只要有人可运行，低一级
  就一个 tick 都拿不到；`nice 0` 把任务放到实时级，它能一直占着 CPU，靠 `Ctrl-C`
  收回（见上）。
- 进程表固定 16 项，窗口槽位按板子算（x86 28 个）。`wait()` 只回传退出码，
  多个子任务并发退出时，退出码不保证和 pid 一一对应（没有 per-child 状态队列，
  也没有僵尸队列）。
- 控制台输入由 tick 轮询（1 ms）而不是 UART 中断：16550 的 16 字节 FIFO 够打字和
  行级粘贴，但高速灌数据会丢；行编辑只有一行的 type-ahead。回显会在"安静 tick"才放行，
  所以后台程序持续输出时，你打的字可能要等一两行才显示出来；键入跨多个 tick 时，
  后台输出仍可能落在两批回显之间（还没有行编辑器，`Ctrl-C`/`Ctrl-D` 不起作用）。
  **type-ahead 只有一行**：`tty_poll()` 把一次读到的整段字符全部喂给行缓冲，第二个
  `\n` 之后的输入被直接丢掉——所以脚本一次灌入多行时，shell 会把它们拼成一条命令
  （`echo a` + `hello` 变成 `echo a hello`）。`tostest compat` 因此对每条命令
  单独启动一次 QEMU。
- 文件系统（littlefs）可读可写，分区 300 KiB / 75 个 4 KiB 块，文件名 ≤ 63 字符。
  写入经过内核侧自检（创建→读回→删除 `selftest.tmp`）和用户态 `wfile` 演示程序验证；
  还没有 `delete`/`rename`/`seek` 系统调用，`read` 也只能顺序读。
- 同时打开的文件数上限是 8（内核句柄池 8、每进程 fd 表 8；fd 3 起）。句柄本身是静态的，
  但每个打开文件那 512 B littlefs 缓存从内核堆动态分配，`close` 时归还，
  所以空闲时几乎不占 RAM。
- x86 上没有任何内存保护：ring 3 只挡住特权指令，用户程序可以直接读写内核内存。
  内核不做用户指针的拷贝校验，只用范围检查 `process_user_ok()`（够用，但不是安全边界）。
  **RV32 / ARM 不同**：PMP / MPU 把 code/data/stack 三段变成硬件强制（见上），
  ELF 加载器侧另有目标窗口校验与 w^X 校验（段同时置 `PF_W|PF_X` 直接拒绝），
  防止坏镜像覆写内核或拿到可写可执行的区间。
- 同一时刻最多 28 个嵌套任务窗口（x86：112 MiB / 4 MiB）、16 个（RV32：16 MiB / 1 MiB）
  或 14 个（ARM：3.5 MiB / 256 KiB），同时活着的任务超过这个数会报 `out of task windows`。
- 内核堆是 **32 KiB** 静态 arena（`.bss`），first-fit **且合并相邻空闲块**；littlefs 的缓存和临时缓冲区都走它
  （每次 `exec` 会临时分配一个文件大小的加载缓冲，随后立即释放）。
- 没有浮点/SIMD（`-mno-sse -mno-80387`）。
- 引导扇区固定读取 180 个扇区（`internal/build/build.go` 的 `KernelSectors`）。改内核大小时
  还要同步改 `boot/stage1.S` 的 `KERNEL_SECTORS` 和 `board/pc/layout.c` 的 `PLAT_LFS_LBA`——
  这两处 Go 代码看不见，所以每次构建都会去源码里读一遍做交叉校验（`internal/build/layout.go`），
  对不上直接报错而不是让镜像悄悄截断或让 littlefs 挂到内核尾巴上。
  另外内核含 `.bss` 的结束地址必须低于 `0x400000`。
- ATA 驱动是 PIO 轮询、28-bit LBA：单次传输最多 255 扇区，适合 QEMU 和真实 IDE；
  没有 48-bit LBA，>128 GiB 的盘不支持。

## 调试

- `tos run -headless -debug`：`build/qemu.log` 里是中断/异常记录。
- 任何 panic 都会同时打到 VGA 和串口。
- `build/kernel.elf` 保留了符号，可以用 `llvm-objdump -d build/kernel.elf` 反汇编。
- 串口输出在窗口模式下写到 `build/serial.log`。

### 内核栈守卫

没有 MMU 就没有 guard page：内核栈往下写穿之后不会 fault，只会继续覆盖 `.bss`，
最后从一片空洞里取指——那是一次和起因毫无关系的野跳。串口驱动曾经在 `'\b'` 上
自己调自己，16 KiB 栈压穿后回绕，崩在 `EIP=4b976946`、`ESP=fffffed4`。

现在栈底有一个 32 位哨兵（`KSTACK_GUARD`，`"STAK"`），tick 每次都检查两件事：

- 当前 trap 帧在 `[__kernel_stack, __kernel_stack_top)` 之内；
- 栈底那个字没变。

任一条不成立就 `KERNEL PANIC: kernel stack overflow`（或
`kernel stack out of bounds: sp=...`）然后停机，而不是让 CPU 跳到空内存。代价是
每 1 ms 一次比较，`.text` 几十字节。

## 许可

MIT
