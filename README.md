# TinyOS

一个用 **C + 汇编** 写的极简 32 位 x86 操作系统，配套的构建/运行工具用 **Go** 写。
它的核心能力是：从 **littlefs** 镜像里**动态加载 ELF 可执行文件**，在 **ring 3** 里运行，
程序可以 `exec` 另一个程序、读写文件、申请堆内存；程序崩溃只会杀掉自己，不会拖垮内核。

```
BIOS -> 自写 MBR 引导扇区 -> 32 位保护模式 -> 内核
     -> 分页 + GDT/IDT/TSS + 定时器 + 键盘
     -> 挂载 littlefs，加载 /bin/sh (ELF32) -> 进入 ring 3
     -> shell 里每条命令都是一个按需加载的 ELF 程序
```

## 功能

- 自写 512 字节 MBR 引导扇区（int 13h LBA 扩展读盘，A20，进入保护模式）
- **ATA PIO 磁盘驱动**（28-bit LBA、轮询无 IRQ）：内核启动后用 `ata_read_sectors`
  /`ata_write_sectors` 直接读写磁盘，不再把文件系统整个拷进内存
- 4 MiB 大页映射内核（0–64 MiB 恒等映射），4 KiB 页映射用户程序
- 每个进程独立地址空间（CR3）、ring 3 + TSS
- ELF32 加载器：解析 `PT_LOAD`、`p_filesz/p_memsz`，分配物理页并映射
- **位置无关程序（PIE）**：用户程序链接为 `ET_DYN`（基址 0），内核加载时整体搬到
  `USER_CODE_BASE` 并应用 `.rel.dyn`/`.rela.dyn` 重定位。这样**同一份二进制可以在
  不同 `APP_BASE` 的板子上运行**（QEMU 与真实 MCU 共用）
- **LZ4 压缩的程序**：用户 ELF 在构建时压缩，内核内嵌约 830 字节的解压器在
  `exec` 时先解压再交给 ELF 加载器（`LZ4TOS` 信封，非压缩文件原样加载）
- 系统调用（`int 0x80`）：`write/read/exec/open/close/readdir/getpid/sleep/uptime/sbrk/exit`
- 同步 `exec` 模型：shell 调用 `exec()` 后阻塞，子程序退出时把退出码还给 shell
- 用户态异常（缺页、GPF、非法指令）只终止当前进程
- **littlefs v2.9.3** 文件系统：构建期用 `mklfs`（同一份 littlefs 源码）打包到磁盘上的
  独立分区，运行时由内核通过 ATA 驱动挂载，支持子目录（`bin/`）、**真实写入**
  （`open/read/readdir` 全部走 littlefs，`prog/erase` 回写磁盘）
- 输出同时走 VGA 文本模式和 COM1，可用 `-headless` 直接在终端里玩
- 其余：PIT 定时器、PS/2 键盘、PIC 重映射、串口

## 目录

```
boot/stage1.S        512 字节引导扇区
kernel/              内核（C + 少量汇编）
  entry.S            32 位入口、清 .bss、建立内核栈
  isr.S              中断/异常入口、进 ring 3 的 iret 收尾
  gdt.c idt.c        描述符表、TSS、PIC
  pmm.c paging.c     物理页分配器、分页
  elf.c               ELF32 加载器
  process.c          进程/地址空间/exec/退出
  syscall.c          系统调用
  ata.c ata.h         ATA PIO 驱动（28-bit LBA，轮询）
  lz4.c lz4.h         LZ4 块解压器（exec 时解压用户程序）
  ramdisk.c           littlefs 挂载与文件访问（块设备回调接 ATA）
  heap.c              内核堆（littlefs 的 malloc/free）
  lfs_config.h        littlefs 的裁剪与配置
  console.c serial.c keyboard.c timer.c
third_party/littlefs/  上游 littlefs v2.9.3（lfs.c/lfs.h/lfs_util.*）
tools/mklfs.c        host 端 littlefs 打包/校验工具
tools/lz4.c          host 端 LZ4 压缩器（只用于打包）
user/                用户态程序（freestanding C）
  crt0.S lib.c syscall.c
  sh.c hello.c echo.c ls.c cat.c crash.c mem.c uptime.c wfile.c reloc.c
  user.ld            PIE 链接脚本（基址 0，由内核重定位）
cmd/tos/             Go 构建工具（build / run / doctor / clean）
internal/build/      编译、ELF->flat、镜像、QEMU
files/               会被打包进 littlefs 分区的普通文件
include/             freestanding 的 stdint/stdbool/string/inttypes 头
```

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
tos> crash          # 故意触发缺页，只会杀掉 crash 自己
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
把 `LFS_MALLOC/LFS_FREE` 接到 `kernel/heap.c` 的 `kmalloc/kfree`；文件句柄用
`lfs_file_opencfg()` 配静态缓存，这样 8 个并发 fd 不需要动态分配。

块设备回调在 `kernel/ramdisk.c`：`bd_read/bd_prog` 用一个 4 KiB bounce 缓冲做
读-改-写（littlefs 可能一次请求整个块），`bd_erase` 把整块写成 0xFF（NOR 语义）。
启动时做一次写自检（创建→读回→删除 `selftest.tmp`），确保 ATA 写路径可用。

### 编译优化等级

默认 `-Os`（`TOS_OPT` 可覆盖）。对本项目来说它比 `-O2` 全面更省：

| | `-O2` | `-Os` |
| --- | --- | --- |
| 内核镜像 | 50,232 B | **38,920 B** |
| 用户程序（9 个） | 18,596 B | **13,528 B** |
| 压缩后分区占用 | 13,569 B | **10,124 B** |

调试时用 `$env:TOS_OPT='-Og'` 或 `-O0` 重新构建即可。

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
解压器在 `kernel/lz4.c`，约 830 字节内核代码，带完整边界检查，损坏的块返回错误
而不是越界访问。

实测用户程序从 18,596 字节压到 13,569 字节（约 73%），单次 `exec` 多花一次
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
| `0x00010000` | 内核镜像（最多 90 KiB）+ `.bss`（约 200 KiB，不占磁盘） |
| `0x0009F000` | 引导期栈 |
| `0x00400000`–`0x04000000` | 物理页分配器管理的 60 MiB |

镜像里**只存 `.text/.rodata/.data`**，`.bss` 的零字节不落盘，由 `kernel/entry.S`
在 `kmain` 之前清零。内核（含 `.bss`）必须结束在 `0x400000`（物理分配器起点）之前，
`tos build` 会检查并打印余量：

```
image 47992 / 92160 bytes, footprint ends at 0x3ce10 (heap at 0x400000, 3852 KiB headroom)
```

没有 ramdisk 了，所以也就不存在实模式 DMA 空洞（`0xA0000`）的限制——文件系统在磁盘上，
内核不再整块拷贝它。

用户虚拟地址：

| 地址 | 内容 |
| --- | --- |
| `0x08048000` | ELF 代码/数据（标准 i386 加载地址） |
| `0x0FFFF000`–`0x10000000` | 8 页用户栈 |
| `0x10000000` 向下 | argv/环境初始栈 |
| `0x20000000` 向上 | `sbrk` 堆 |

内核同时把 0–64 MiB 恒等映射（4 MiB 大页，`U/S=0`），因此内核可以直接访问物理页，
而 ring 3 无法碰内核内存。

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
- 内核不做用户指针的完整拷贝校验，只用页表检查 `paging_user_ok()`（够用，但不是安全边界）。
- 内核堆只有 64 KiB 静态 arena，first-fit **且合并相邻空闲块**；littlefs 的缓存和临时缓冲区都走它
  （每次 `exec` 会临时分配一个文件大小的加载缓冲，最大约 24 KiB，随后立即释放）。
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
