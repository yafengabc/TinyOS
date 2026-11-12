# PC / x86（`-target x86`）

传统 BIOS + MBR 启动的 32 位 x86 目标，也是仓库的默认目标。日常在 QEMU 里跑，
产物 `build/tinyos.img` 同时就是一块可以写进 U 盘/磁盘的真机镜像。

## 硬件与启动链

| 项 | 值 |
| --- | --- |
| CPU | i386 兼容，单核（`PLAT_MAX_CPUS = 1`） |
| 启动 | BIOS 读 LBA 0 的 512 B 引导扇区（`boot/stage1.S`），自己进保护模式 |
| 内核 | 链接在 `0x10000`，由引导扇区用 int 13h LBA 扩展读入 |
| 文件系统 | ATA PIO 上的 littlefs，从 LBA 181 开始，随镜像发布 300 KiB（75 块） |
| 内存 | QEMU 给 128 MiB（`PLAT_RAM_TOP = 0x08000000`），平坦地址空间 |
| 控制台 | VGA 文本模式 + COM1；`-headless` 时只用 COM1 |
| 输入 | PS/2 键盘 |

镜像布局：`LBA 0` 引导扇区 → `LBA 1..180` 内核（90 KiB）→ `LBA 181` 起 littlefs。

## 构建与运行

```bash
go run ./cmd/tos build -target x86        # -> build/tinyos.img
go run ./cmd/tos run   -target x86        # QEMU 窗口 + build/serial.log
go run ./cmd/tos run   -target x86 -headless   # 串口直接在终端里
go run ./cmd/tos run   -target x86 -debug      # build/qemu.log 里有中断/复位跟踪
```

QEMU 参数固定在 `internal/build/qemu.go`：`-m 128M`、`tinyos.img` 走 IDE、`-boot c`。

真机：把 `build/tinyos.img` 原样写到磁盘或 U 盘（`dd`/Rufus 的 DD 模式），
用**传统 BIOS/CSM** 启动；UEFI-only 的机器不行。

## 移植进度

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| 引导、保护模式、GDT/IDT/TSS | ✅ | 自写 MBR，不依赖 multiboot |
| ring 3 用户态 + `int 0x80` | ✅ | 任务崩溃只杀自己 |
| ATA PIO + littlefs 读写 | ✅ | 内核直连磁盘，不整盘拷贝 |
| ELF32 PIE 加载 + LZ4 流式解压直写窗口 | ✅ | 同一份 `bin/hello` 可跨板 |
| 抢占调度、`exec/wait/pipe/dup2/poll` | ✅ | 与其它目标共用同一套内核 |
| `/proc`、`/dev`、线程、邮箱、内核裁剪 | ✅ | 见根 README 的功能列表 |
| 内存保护 | — | **没有**：关分页（CR0.PG=0），只有 ring 3 特权级隔离；`prot` 会打印 `survived: this machine cannot stop us` |
| SMP | — | 单核硬件 |

x86 是回归基准目标（`Makefile` 的 `run`/`headless` 默认就是它），
`tostest smoke`/`compat` 也把它当作黄金参考。

## 已知限制

- 只有 32 位、无分页、无 PAE；地址空间与内核完全共享。
- ATA 只做 LBA28 PIO 轮询，没有中断/DMA。
- 文件系统镜像随内核走，扩容靠 littlefs 自身；没有分区表。
- 没有 USB/网络驱动，相关用户程序在 x86 上会被构建配置排除。

## 相关文件

- `boot/stage1.S`、`boot/stage1.ld`：引导扇区
- `arch/x86/`：`entry.S`、`isr.S`、`gdt.c`、`idt.c`、`ata.c`、`serial.c`、`timer.c`、`keyboard.c`
- `board/pc/layout.c`：内存与 littlefs 分区常量
- `internal/build/build.go`：`x86` 目标、`build/tinyos.img` 打包
