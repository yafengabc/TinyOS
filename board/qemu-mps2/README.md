# QEMU mps2-an385（`-target arm`）

Cortex-M3 目标，用来在 QEMU 上验证 ARM 侧的异常/PSP/SysTick 和 ARMv7-M MPU，
是 RP2350（M33）之前的 ARM 参考实现。

## 硬件与启动链

| 项 | 值 |
| --- | --- |
| CPU | Cortex-M3（ARMv7-M），单核 |
| 机器 | `qemu-system-arm -machine mps2-an385` |
| 代码 | 链接在 `0x0`，QEMU 按 ELF 段放置；向量表就在 0 |
| 数据/BSS/内核栈 | 4 MiB SRAM @ `0x20000000` |
| 文件系统 | **内嵌** littlefs ramdisk 600 KiB（1200 扇区）在 `.data` |
| 控制台 | CMSDK UART0，115200 8N1 |
| 定时器 | SysTick，1 kHz |
| 内核堆 | 128 KiB |

## 构建与运行

```bash
go run ./cmd/tos build -target arm
go run ./cmd/tos run   -target arm
go run ./cmd/tos run   -target arm -headless
```

`tos run` 实际执行：`qemu-system-arm -machine mps2-an385 -kernel build/kernel-arm.elf`，
`-headless` 时加 `-serial stdio`，否则串口写 `build/serial.log`（VGA 窗口里没有输出）。

## 移植进度

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| 向量表、复位、`.data/.bss` 初始化 | ✅ | `arch/arm/start.S`，三块板共用 |
| Handler/MSP + Thread/PSP 双栈 | ✅ | 用户任务跑在非特权 Thread 模式 |
| `svc` 系统调用、SysTick 抢占 | ✅ | 只在用户态帧上抢占 |
| ARMv7-M MPU 每任务区域 | ✅ | 代码 RX / 数据+堆 RW / 栈 RW / 栈下护栏 |
| UART0 收发、内嵌 littlefs | ✅ | |
| 线程、邮箱、`poll`、`/proc`、`/dev` | ✅ | 与其它目标同一套内核 |
| FPU / 双核 / 真机 | — | mps2-an385 是单核软核，没有 FPU 需求 |

`arm` 是 ARM 移植的基线：RP2040（M0+，无 MPU）和 RP2350（M33，PMSAv8）都是在
这套 `arch/arm` 上改 `TOS_ARM_M0P` / `TOS_ARM_MPU_V8` 分支得到的。

## 已知限制

- 没有真机对应，只服务 QEMU；外设只有 CMSDK UART。
- ramdisk 在 RAM 里，写入不持久。
- 单核、无 SMP 路径。

## 相关文件

- `arch/arm/start.S`：向量表、`_start`、SVC/PendSV/SysTick 入口
- `arch/arm/platform.c`：MPU 编程、任务切换、UART
- `arch/arm/scs.h`、`arm_memmap.h`：SCB/NVIC/SysTick 寄存器
- `board/qemu-mps2/layout.c`：内存与 ramdisk 常量
