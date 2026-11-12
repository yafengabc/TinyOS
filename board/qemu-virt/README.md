# QEMU virt（`-target rv32` / `rv32smp` / `rv32nc`）

RV32 的主力目标：`qemu-system-riscv32 -machine virt -bios none -kernel`，内核直接链接在
RAM 起始地址 `0x80000000`，不经过 OpenSBI。

## 硬件与启动链

| 项 | 值 |
| --- | --- |
| CPU | RV32IMC（`rv32`/`rv32smp`）、RV32IM（`rv32nc`，无压缩指令） |
| 核数 | 1（`rv32`、`rv32nc`）；2（`rv32smp`，加 A 扩展与自旋锁） |
| 启动 | QEMU 把 ELF 放到链接地址，`_start` 在 M 模式建立 trap 向量后进 U-mode |
| RAM | 内核只用前 128 MiB（`PLAT_RAM_TOP = 0x88000000`） |
| 文件系统 | **内嵌**在 `.data.ramdisk` 里的 littlefs（随镜像发布 300 KiB） |
| 控制台 | 16550 UART0，`-serial stdio` 或写入 `build/serial.log` |
| 定时器 | CLINT `mtime`/`mtimecmp`，每 hart 一个 1 kHz tick |

## 构建与运行

```bash
go run ./cmd/tos build -target rv32
go run ./cmd/tos run   -target rv32
go run ./cmd/tos run   -target rv32 -headless     # 串口接终端

go run ./cmd/tos build -target rv32smp
go run ./cmd/tos run   -target rv32smp            # 自动加 -smp 2
```

`rv32nc` 只是换 ISA，`tos run` 没有为它单独开分支，手动跑：

```bash
go run ./cmd/tos build -target rv32nc
qemu-system-riscv32 -machine virt -m 256M -bios none \
    -kernel build/kernel-rv32nc.elf -serial stdio
```

`tos run` 生成的参数是 `-m 256M -smp N -serial file:build/serial.log`（`-headless` 换成 stdio），
加 `-debug` 会写 `build/qemu.log`。

## 移植进度

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| M 模式启动、trap、U-mode + `ecall` | ✅ | 系统调用号与 x86 共用 |
| PMP 分段保护 | ✅ | 每任务 code(RX) / data+heap(RW) / stack(RW) 三条 TOR 区域，越界当场 fault |
| CLINT 定时器、16550、内嵌 littlefs | ✅ | |
| 抢占调度、`exec`、管道、`poll`、线程、邮箱 | ✅ | 与 x86 同源 |
| SMP（2 hart） | ✅ | `amoswap` 自旋锁、per-hart 内核栈/mtvec/mtimecmp |
| `rv32nc`（无 RVC） | ✅ | 用来对比压缩指令的体积/性能，行为与 `rv32` 相同 |
| WiFi/网络 | — | QEMU 上没有对应硬件，相关程序默认不打包 |

`tostest smoke`、`tostest compat` 的 RV32 一侧都在这个目标上跑；`rv32emu-c/`
子模块模拟的也是同一套 RV32IMC 指令，`tostest emu`/`emutest` 用它做全功能回归。

## 已知限制

- 用户区固定 16 MiB（`PLAT_APP_MAX`），64 KiB 粒度；比真机板宽松得多，不代表实际预算。
- 文件系统在 `.data` 里，写入只落在 RAM，重启即复原。
- 没有 MMU/S-mode，内核始终在 M 模式。

## 相关文件

- `arch/rv32/start.S`：入口、trap 向量、第二 hart 的 `_start` 分支
- `arch/rv32/platform.c`：UART/CLINT/PMP/自旋锁
- `board/qemu-virt/layout.c`：内存与 ramdisk 常量
- `internal/build/qemu.go`：QEMU 启动参数
