# TinyOS

一个用 **C + 汇编** 写的极简 32 位 x86 操作系统，配套的构建/运行工具用 **Go** 写。
它的核心能力是：从 **littlefs** 镜像里**动态加载 ELF 可执行文件**，在 **ring 3** 里运行，
程序可以 `exec` 另一个程序、读写文件、申请堆内存；程序崩溃只会杀掉自己，不会拖垮内核。

```
BIOS -> 自写 MBR 引导扇区 -> 32 位保护模式 -> 内核
     -> GDT/IDT/TSS（平坦模型，不启用分页）+ 定时器 + 键盘
     -> 挂载 littlefs，加载 /bin/sh (ELF32) -> 进入 ring 3
     -> shell 里每条命令都是一个按需加载的 ELF 程序
```

## 功能

- 自写 512 字节 MBR 引导扇区（int 13h LBA 扩展读盘，A20，进入保护模式）
- **ATA PIO 磁盘驱动**（28-bit LBA、轮询无 IRQ）：内核启动后用 `ata_read_sectors`
  /`ata_write_sectors` 直接读写磁盘，不再把文件系统整个拷进内存
- **平坦地址空间、不启用分页**（CR0.PG=0）：内核与用户程序共享一条 4 GiB 直线地址
  空间，`ring 3` + TSS 只提供特权级隔离、不提供内存保护——这与 MCU 目标
  （ESP32-C3 / RP2040）的能力边界一致
- **每任务窗口**：`exec` 时从 `[APP_BASE, APP_MAX)` 里按窗口大小（x86 为 4 MiB）
  分配一块，镜像在底部、`sbrk` 堆在中部、栈在顶部；子进程退出时 LIFO 回收，
  因此同步 `exec` 不会覆盖父进程（shell 自己的代码始终完好）
- ELF32 加载器：解析 `PT_LOAD`、`p_filesz/p_memsz`，把镜像拷进任务窗口并清零 `.bss`
- **位置无关程序（PIE）**：用户程序链接为 `ET_DYN`（基址 0），内核加载时整体搬到
  该任务的窗口基址并应用 `.rel.dyn`/`.rela.dyn` 重定位。这样**同一份二进制可以在
  不同 `APP_BASE` 的板子上运行**（QEMU 与真实 MCU 共用）
- **LZ4 压缩的程序**：用户 ELF 在构建时压缩，内核内嵌约 420 字节的解压器在
  `exec` 时先解压再交给 ELF 加载器（`LZ4TOS` 信封，非压缩文件原样加载）
- 系统调用（`int 0x80`）：`write/read/exec/open/close/readdir/getpid/sleep/uptime/sbrk/exit`
- 同步 `exec` 模型：shell 调用 `exec()` 后阻塞，子程序退出时把退出码还给 shell
- 用户态异常（非法指令、GPF）只终止当前进程
- **littlefs v2.9.3** 文件系统：构建期用 `mklfs`（同一份 littlefs 源码）打包到磁盘上的
  独立分区，运行时由内核通过 ATA 驱动挂载，支持子目录（`bin/`）、**真实写入**
  （`open/read/readdir` 全部走 littlefs，`prog/erase` 回写磁盘）
- 输出同时走 VGA 文本模式和 COM1，可用 `-headless` 直接在终端里玩
- 其余：PIT 定时器、PS/2 键盘、PIC 重映射、串口

## 目录

```
boot/stage1.S        512 字节引导扇区
kernel/              架构无关内核（C）
  main.c             启动、ramdisk 自检、拉起 /bin/sh
  platform.h         HAL：各 arch/board 实现的平台接口
  process.c syscall.c  进程/exec/系统调用
  elf.c               ELF32 加载器（PT_LOAD + REL/RELA 重定位，目标窗口校验）
  ramdisk.c           littlefs 挂载与文件访问（块设备回调接 ATA）
  heap.c lz4.c libgcc.c console.c string.c
  lfs_config.h        littlefs 的裁剪与配置
arch/x86/            x86 实现：entry.S isr.S gdt idt ata serial timer keyboard platform.c
arch/rv32/           RV32 实现（P1 桩：start.S + 平坦平台骨架，尚不能跑用户程序）
board/pc/            x86 板级内存布局（layout.c）
board/qemu-virt/     QEMU virt (rv32) 板级布局（P2，布局待重排）
docs/PLATFORM-DESIGN.md  无 MMU 平坦模型的设计与路线图
third_party/littlefs/  上游 littlefs v2.9.3（lfs.c/lfs.h/lfs_util.*）
tools/mklfs.c        host 端 littlefs 打包/校验工具
tools/lz4.c          host 端 LZ4 压缩器（只用于打包）
user/                用户态程序（freestanding C）
  crt0.S lib.c syscall.c
  sh.c hello.c hello_printf.c echo.c ls.c cat.c crash.c mem.c uptime.c wfile.c reloc.c
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

### 4. 检查

```powershell
go run ./cmd/tos doctor
```

应能看到工具链和 qemu-system-i386 的路径与版本。

## 使用

```powershell
# 编译内核 + 用户程序 + littlefs 分区，生成 build/tinyos.img
go run ./cmd/tos build

# 在 QEMU 窗口里启动（VGA 文本 + PS/2 键盘）
go run ./cmd/tos run

# 无窗口，直接用终端当串口控制台（推荐，能复制粘贴命令）
go run ./cmd/tos run -headless

# 调试：把中断/CPU 复位记录到 build/qemu.log
go run ./cmd/tos run -headless -debug
```

进去以后：

```
tos> ls
tos> cat readme.txt
tos> hello one two three
tos> mem
tos> crash          # 故意执行非法指令，只会杀掉 crash 自己
tos> uptime
tos> run echo hi
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
> 自动化测试请用 `scripts/smoke.ps1`，它通过 TCP 串口喂命令：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/smoke.ps1
```

脚本会启动 QEMU、执行 `ls / cat / hello / mem / crash / uptime / exit`，
检查提示符和内核 panic，最后打印完整记录。

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

普通数据文件放到 `files/` 下（可以带子目录），会以同样的相对路径出现在 littlefs 里，
用 `cat` 读取。

## 文件系统

文件系统是 [littlefs](https://github.com/littlefs-project/littlefs) v2.9.3，
源码 vendor 在 `third_party/littlefs/`，**住在磁盘上**（ATA 分区的 LBA 181 起，300 KiB）。
构建流程：

1. `tos build` 把要打包的文件暂存到 `build/fs-stage/`
2. 用 Zig 把 `tools/mklfs.c` + `lfs.c` + `lfs_util.c` 编成 host 程序 `build/mklfs.exe`
3. `mklfs.exe` 用 littlefs 自己 `lfs_format` + `lfs_file_write` 生成 `build/ramdisk.bin`
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
| 内核镜像 | 42,132 B | 34,916 B | **32,852 B** |
| 用户程序（11 个） | 16,760 B | 15,084 B | **14,476 B** |
| 压缩后分区占用 | 11,533 B | 10,714 B | **10,305 B** |

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

实测用户程序（11 个）从 14,476 字节压到 9,807 字节（约 68%），单次 `exec` 多花一次
内存分配和 memcpy 级别的时间。

手动检查镜像内容：

```powershell
.\build\mklfs.exe -v .\build\ramdisk.bin
```

## 磁盘布局

| LBA | 内容 |
| --- | --- |
| 0 | 引导扇区（512 字节） |
| 1 – 180 | 内核镜像（最多 90 KiB） |
| 181 – 780 | littlefs 分区（600 扇区 = 300 KiB） |

引导扇区只读内核（`int 13h`），**不再加载文件系统**；内核起来后由 ATA 驱动按需读盘。
LBA 常量在 `boot/stage1.S`、`internal/build/build.go`、`kernel/ramdisk.h`
三处同步，构建时对内核镜像大小和 `.bss` 结尾都有检查。

## 内存布局

物理内存 / 引导期：

| 地址 | 内容 |
| --- | --- |
| `0x00007C00` | 引导扇区 |
| `0x00010000` | 内核镜像（最多 90 KiB）+ `.bss`（约 62 KiB，不占磁盘） |
| `0x0009F000` | 引导期栈 |

镜像里**只存 `.text/.rodata/.data`**，`.bss` 的零字节不落盘，由 `kernel/entry.S`
在 `kmain` 之前清零。内核（含 `.bss`）必须结束在 `0x400000` 之前，
`tos build` 会检查并打印余量：

```
image 32852 / 92160 bytes, footprint ends at 0x27984 (limit 0x400000, 3937 KiB headroom)
```

没有 ramdisk 了，所以也就不存在实模式 DMA 空洞（`0xA0000`）的限制——文件系统在磁盘上，
内核不再整块拷贝它。

用户地址（平坦模型，全部是物理地址）：

| 地址 | 内容 |
| --- | --- |
| `0x01000000`–`0x08000000` | 任务窗口区，每个任务独占 4 MiB（`PLAT_APP_WINDOW`） |
| 窗口 + 0 | ELF 代码/数据（PIE 重定位到这里） |
| 窗口 + 镜像大小 向上 | `sbrk` 堆（上限到栈底） |
| 窗口顶部 − 32 KiB 到顶 | 8 页用户栈，栈顶留 256 B 余量给 argv |

没有分页、没有 CR3 切换：`exec` 只是把镜像拷到窗口里再把栈指针指好，退出时把
bump 指针退回窗口起点。**代价是没有任何内存保护**——用户程序能读写内核内存，
这是 MCU 级 OS 的刻意取舍，见 `docs/PLATFORM-DESIGN.md`。

## 系统调用 ABI

`eax` = 调用号，`ebx/ecx/edx` = 参数 1–3，返回值在 `eax`，`int 0x80`。

| # | 名称 | 说明 |
| --- | --- | --- |
| 0 | exit | 终止当前进程，退出码放 `ebx` |
| 1 | write | `write(fd, buf, len)`，fd 1/2 = 控制台，fd ≥ 3 写文件 |
| 2 | read | `read(0,...)` 按行读取（带回显），fd ≥ 3 读文件 |
| 3 | exec | `exec(path, argv, argc)`，同步执行并返回子进程退出码 |
| 4 | open | 打开 littlefs 文件（只读），返回 fd（≥ 3） |
| 5 | close | 关闭 fd |
| 6 | readdir | `readdir(index, buf, len)`，枚举磁盘上的文件（含 `bin/` 前缀） |
| 7 | getpid | 当前 pid |
| 8 | sleep | 毫秒 |
| 9 | uptime | 开机毫秒数 |
| 10 | sbrk | 扩展堆，返回旧 brk（0 表示失败） |
| 11 | create | `create(path)` 创建/截断文件并返回可写 fd（≥ 3） |

用户程序可以直接往磁盘写文件了，例如：

```
tos> wfile demo.txt hello from ATA disk
wrote demo.txt
tos> cat demo.txt
hello from ATA disk
```

写入是**持久的**：重启后 `demo.txt` 依然存在。

## 已知限制（有意保持简单）

- 单核、无抢占：同一时刻只有一个用户进程在跑，`exec` 是同步的（shell 等待子程序）。
  定时器中断已就绪，想加抢占式调度可以从 `process.c` 的进程表开始。
- 文件系统（littlefs）可读可写，分区 300 KiB / 75 个 4 KiB 块，文件名 ≤ 63 字符。
  写入经过内核侧自检（创建→读回→删除 `selftest.tmp`）和用户态 `wfile` 演示程序验证；
  还没有 `delete`/`rename`/`seek` 系统调用，`read` 也只能顺序读。
- 同时打开的文件数上限是 8（内核句柄池 8、每进程 fd 表 8；fd 3 起）。句柄本身是静态的，
  但每个打开文件那 512 B littlefs 缓存从内核堆动态分配，`close` 时归还，
  所以空闲时几乎不占 RAM。
- 内核不做用户指针的完整拷贝校验，只用范围检查 `process_user_ok()`（够用，但不是安全边界）。
  ELF 加载器侧有目标地址窗口校验（段与重定位写入必须落在当前任务的窗口内），
  防止坏镜像覆写内核。**无内存保护是刻意设计**：用户程序可以直接读写内核内存。
- 同一时刻最多 28 个嵌套任务窗口（112 MiB / 4 MiB），同步 `exec` 嵌套超过这个深度会
  报 `out of task windows`。
- 内核堆是 **32 KiB** 静态 arena（`.bss`），first-fit **且合并相邻空闲块**；littlefs 的缓存和临时缓冲区都走它
  （每次 `exec` 会临时分配一个文件大小的加载缓冲，随后立即释放）。
- 没有浮点/SIMD（`-mno-sse -mno-80387`）。
- 引导扇区固定读取 180 个扇区；改内核大小时同步改 `boot/stage1.S`、
  `internal/build/build.go`、`kernel/ramdisk.h` 三处（`KERNEL_SECTORS` /
  `KernelSectors` / `LFS_PARTITION_LBA`），并保持内核含 `.bss` 的结束地址低于 `0x400000`。
- ATA 驱动是 PIO 轮询、28-bit LBA：单次传输最多 255 扇区，适合 QEMU 和真实 IDE；
  没有 48-bit LBA，>128 GiB 的盘不支持。

## 调试

- `tos run -headless -debug`：`build/qemu.log` 里是中断/异常记录。
- 任何 panic 都会同时打到 VGA 和串口。
- `build/kernel.elf` 保留了符号，可以用 `llvm-objdump -d build/kernel.elf` 反汇编。
- 串口输出在窗口模式下写到 `build/serial.log`。

## 许可

MIT
