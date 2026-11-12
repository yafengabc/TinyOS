# RP2350 Hazard3 / RISC-V（`-target rp2350rv` / `rp2350rvsmp`）

同一块 RP2350 板子，把它切成两颗 **Hazard3 RV32IMAC** 核来跑。ARM 与 RISC-V 是
**两张不同的镜像**：PICOBIN 的 IMAGE_TYPE 决定 ROM 从哪套入口起，不是运行时切换。

## 硬件与启动链

| 项 | 值 |
| --- | --- |
| CPU | Hazard3 RV32IMAC（M 模式唯一），单 hart 或双 hart |
| SRAM | 520 KiB：前 128 KiB 内核，其后 384 KiB = 24 个 16 KiB 窗口 |
| Flash | 4 MiB QSPI，XIP @ `0x10000000`；`.data` 由 `_start` 从 LMA 拷到 SRAM |
| 启动 | ROM 扫前 4 KiB 找 `board/rp2350rv/image_def.S` 的 PICOBIN（IMAGE_TYPE `0x1101` = EXE\|CPU_RISCV\|CHIP_RP2350），只跳到 ENTRY_POINT，**不搬任何段** |
| UF2 | family `0xe48bff5a`（RISC-V Secure EXE），产物 `build/rp2350rv.uf2` |
| 文件系统 | littlefs @ flash `0x80000`，镜像仍是 300 KiB（75 块），暂不拉伸 |
| 任务窗口 | 16 KiB 粒度；窗口必须 2 的幂对齐（Hazard3 PMP 不支持 TOR） |
| 内存保护 | **有**：PMP NAPOT，每任务一个对齐窗口，U-mode 只能碰自己的 code/data/heap/stack |
| 定时器 | Hazard3 machine timer：`mtime`/`mtimecmp` 在 SIO 块 `0xd0000000 + 0x1b0`，`mtimecmp` 是 core-local |

## 构建与烧写

```bash
go run ./cmd/tos build -target rp2350rv
# -> build/kernel-rp2350rv.elf, build/ramdisk-rp2350rv.bin, build/rp2350rv.uf2

go run ./cmd/tos build -target rp2350rvsmp   # 双 hart
```

烧写和 ARM 镜像完全一样（BOOTSEL 盘、`scripts/rp2350/flash.py`）；只是 UF2 family
不同，所以**必须用 `build/rp2350rv.uf2`**，把 ARM 的 `build/rp2350.uf2` 拖进去
ROM 会接受块但不启动。

```bash
python scripts/rp2350/flash.py --uf2 build/rp2350rv.uf2 --smoke
```

## 控制台

- **UART0**（GPIO0/1，115200）——CH343P / USB-TTL。
- **USB CDC**：与 ARM 侧同一套 TinyUSB 设备栈，区别是 Hazard3 的外部中断走自己的
  CSR，所以这里**轮询**（`usb_cdc_poll`）而不是 IRQ。枚举期间
  `usb_cdc_console_init()` 的紧循环负责控制传输，之后 CPU 0 在 console idle/tick
  里排水。
- SMP 下 shell 可能被调度到 CPU 1，所以 `usb_cdc_putc()` 写的是 1 KiB 跨核 TX ring
  （短自旋锁 + 本核关中断），由 CPU 0 排空。

## 移植进度

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| PICOBIN IMAGE_DEF + XIP + `.data` 拷贝 | ✅ 实机 | `[A] pre-pll ok` → `[B] clk=0x08f0d568` |
| PLL_SYS 150 MHz（先切走 clk_sys 再动 PLL） | ✅ 实机 | 顺序错会让 CPU 当场停、UART 一个字节都没有 |
| PMP NAPOT 每任务窗口 | ✅ 实机 | `prot stack` 越界写进 shell 窗口被杀，证明边界生效 |
| UART0 + USB CDC 双控制台 | ✅ 实机 | COM6 = UART0，COM7 = USB CDC |
| littlefs on flash（ROM RISC-V 表 0x7dfa） | ✅ 实机 | `ls`/`df`、`dd` 写读校验、`rm` 后 df 复原 |
| 单 hart `rp2350rv` | ✅ 实机 | 46 个文件、`py pytest.py` 54 项全过 |
| 双 hart `rp2350rvsmp` | ✅ 实机 | `[smp] 2 of 2 CPU(s) online`、`smpbench` 1.89–2.01x |
| 管道、`sleep`、`reboot`、`piperace` | ✅ 实机 | 见 `docs/OPEN-ITEMS.md` §25 验收表 |
| 分区拉伸到 3.5 MiB | ☐ | 现在固定 75 块，拉伸是后续工作 |

## 已知限制

- 窗口内部不再细分 code/data/guard（要细分得让镜像的 `wx` 落在对齐边界上）；
  和 RP2040 的 flat 模型同一个粒度。
- `rp2350rvsmp` 的第二 hart 从 ROM 的 SIO-FIFO 等待循环里拉起来，握手是
  pico-sdk 的 `{0,0,1,mtvec,sp,_start}`，`sev` 在 Hazard3 上是 `h3.unblock`
  （`slt x0,x0,x1`），写成普通 fence 会超时只上线 1 个核。
- 没有 MMU；内核始终在 M 模式，用户程序在 U 模式。
- 只支持 Hazard3 这一种 RISC-V 配置，别把 RV32 代码直接搬去别的板子。

## 相关文件

- `board/rp2350rv/{platform.c,layout.c,kernel.ld,image_def.S,board_devs.c,board_internal.h,rp2350rv_memmap.h}`
- `drivers/gpio/rp2350rv/`、`drivers/spi/rp2350rv/`、`drivers/display/st7789/rp2350rv/`、`drivers/usb/rp2350rv/`
- `arch/rv32/start.S`（`TOS_COPY_DATA`、第二 hart 分支）、`arch/rv32/platform.c`（PMP NAPOT 分支）
- `internal/build/uf2.go`：`emitRP2350RVUF2()`、family `0xe48bff5a`、ENTRY_POINT item
