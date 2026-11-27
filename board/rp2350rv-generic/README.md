# RP2350 芯片基线 / Hazard3 RISC-V（`-target rp2350rv-generic`）

和 [`rp2350-generic`](../rp2350-generic/README.md) 是**同一颗硅片的另一半**：同样的
SRAM 图、同样的控制台、同样的 RAM disk，换成 Hazard3（RV32IMAC）两条 hart 和 PMP。
ARM 那一层的 `layout.c`、`board_internal.h`、设备表驱动的 LED/按键代码都被继承，
本目录只提供带 ISA 的三样东西：`platform.c`、`kernel.ld`、`image_def.S`，外加 PMP 的
映射算法。

具体板子在这层上再加板级数据，比如
[`rpi-pico2-rv32/`](../rpi-pico2-rv32/README.md)。

## 硬件与启动链

| 项 | 值 |
| --- | --- |
| CPU | 双 Hazard3 hart（RV32IMAC，M/U 两态），PLL_SYS 150 MHz；镜像默认单 hart |
| SRAM | 520 KiB @ `0x20000000`：前 128 KiB 内核，其后 384 KiB 任务窗口 |
| Flash | **不声明**（`fs_backing = embedded`）：文件系统是内核里的 RAM disk |
| 启动 | 与 ARM 侧同一套 PICOBIN：ROM 扫前 4 KiB 找 IMAGE_DEF，从偏移 0 的向量表取栈/入口；没有第二级加载器 |
| UF2 | family `0xe48bff59`，产物 `build/tinyos-rp2350rv-generic.uf2`（约 833 KiB（852,480 B）） |
| 文件系统 | RAM disk，300 KiB（75 块），掉电即失 |
| 任务窗口 | **16 KiB** 粒度，24 个窗口；栈 4 KiB、护栏 4 KiB、`stack_align = 1` |
| 内存保护 | **有**：PMP（NAPOT），代码 RO+X、数据/堆 RW、栈 RW，堆与栈之间留洞 |
| 内核堆 | 32 KiB（`targets/rp2350rv-generic.conf`） |

### 为什么窗口是 16 KiB 而 ARM 侧是 4 KiB

Hazard3 的 PMP 只支持 NAPOT/NA4，不支持 TOR（`pmpcfg` 的 `A=1` 被 WARL 成 OFF），
所以每一段窗口要拆成若干个自然对齐的二次幂块。栈必须是**自己的一块**，基址按大小
对齐，于是内核要求栈提示值整除窗口粒度。板上自带的 `csc`/`rcc` 要 16 KiB 栈，
所以这里的粒度是 16 KiB（384 KiB / 16 KiB = 24 个窗口），`stack_align = 1`。

代价是块预算：8 个软件可写条目里 5 号是 trap vector，只剩 7 块要同时装下代码段、
数据+堆、栈。窗口边界按页切，一段 `wx` 位置不巧就会拆出两段各带小尾巴的分解
（issue #44 的 `rcc`：3 + 4 + 1 = 8 块，差一块）。`pmp_map()` 因此按"梯形策略"
逐级让出最少的保护让映射塞得下，并报告用了哪一级，端口只打印一次。

### 取指跨两个 PMP 条目的规则

RISC-V 规定一次访问只有在**单个**条目覆盖它时才算匹配。带 RVC 时 32 位指令可以落在
`page_end-2`，一条取指横跨两个条目，即使两边都给 X —— 必须 fault。所以
`board.conf` 里是 `rvc = auto`：先按压缩指令构建，只对镜像会跨页的程序重新构建成
不带 RVC 的版本（`internal/build/codepage.go`），`msh` 这类不受影响的保留压缩。

### M 态不受 PMP 约束

`mret` 回到 M 态的内核线程不受 PMP 限制（没有条目设 lock 时），所以 `prot` 只对
普通用户进程有意义，`kthreadtest` 的保护用例在 RV32 上看到的是设计行为而不是漏洞。

## 构建

```bash
go run ./cmd/tos build -target rp2350rv-generic
# -> build/kernel-rp2350rv-generic.elf
# -> build/tinyos-rp2350rv-generic.uf2
```

构建日志里可能有若干行：

```text
RVC bin/edit / pio / sh / csc / rcc / rvm: rebuilt without compressed instructions
```

那是 `rvc = auto` 在为跨页的程序换编码，不是失败。

## 烧写与看日志

```bash
python scripts/rp2350/flash.py --list
python scripts/rp2350/flash.py --uf2 build/tinyos-rp2350rv-generic.uf2 --smoke
python scripts/rp2350/flash.py --uf2 build/tinyos-rp2350rv-generic.uf2 --smoke -p COM19
python scripts/rp2350/bootsel_poke.py COM27 20 --dtr   # 不按键进 BOOTSEL（固件支持 ROM 复位时）
```

`--port auto` 按 VID/PID 找 CDC（`2e8a:000a`）。刷机后 **UART0 不一定有字节**（这块
芯片上两个 ISA 都见过），抓启动日志以 CDC 为准；`flash.py` 默认就是这么做的。

Hazard3 镜像的启动标记和 ARM 不同：`[A]` 打印的是时钟快照（FC0 实测的 clk_sys），
不是 ARM 侧的 SAU/MPU 摘要。`accept33.py --image rpi-pico2-rv32` 之类按 `rv=True`
选这套用词。

## 控制台与外设

- USB CDC（`2e8a:000a`）+ UART0（GP0/GP1，115200 8N1），两路都是控制台。
- 基线没有 LED/按键（设备表为空）；`/dev/led0`、`/dev/key0` 不存在。内核心跳仍写死
  **GP3**，没接 LED 的板子上看不到。
- `gpio`/`spi`/`i2c`/`pwm`/`dma`/`pio` 用户命令随镜像编译；`cat /proc/dev` 看设备表，
  `cat /proc/cpuinfo` 看 hart 数与时钟。PMP 用了哪一级策略只在启动日志里打印一次。

## 移植进度

保守口径：✅ = 真机确认过；◐ = 代码已实现、构建通过，但还没有独立验收；☐ = 未做。

| 项目 | 状态 | 说明 |
| --- | --- | --- |
| 构建 + UF2（PICOBIN / family） | ✅ | 约 833 KiB（852,480 B）；`tos build all` 覆盖，构建期校验 IMAGE_DEF/MSP/入口对齐 |
| 冷启动到 shell | ◐ | 本 target 无独立验收卡；同一 `platform.c` 被 `rpi-pico2-rv32*` 使用并实测到 shell |
| 双 hart 启动（SIO FIFO） | ◐ | 本 target 单 hart；双 hart 在 `rpi-pico2-rv32-smp` 上验（`smpbench` 2.00x） |
| PMP 映射（NAPOT / 梯子策略） | ◐ | `pmp_map()` 有主机侧 harness（`internal/build/testdata/pmp_map_harness.c`）；板级实现在 Pico 2 RV32 上验 |
| `prot`（heap/stack/dev fault） | ◐ | RV32 的 fault 用词是 `load/store access fault`；rodata/null 按设计存活 |
| RVC 逐程序回退 | ◐ | `pico2-matrix` 卡记录 6 个程序回退、`msh` 保留压缩 |
| littlefs on RAM disk | ◐ | 同一 `ramdisk_init()`；本镜像未单独跑 `dd` |
| USB CDC | ◐ | RV32 侧有自己的 `usbd_app_driver_get_cb` 定义（medany 下弱符号会链成非零地址，见 `drivers/usb/rp2350rv/usb_cdc.c`） |
| tick（mtime） | ◐ | `TIMER0` 必须先从复位里放出来并设 `CYCLES`，否则 mtime 不走、控制台环形缓冲不排空，看起来像死机 |

## 已知限制

- 单 hart 镜像；双 hart 只有具体板子的 `-smp` target。
- RAM disk 掉电即失；持久化要在板级 conf 里声明 flash。
- 16 KiB 窗口意味着一块窗口里放不下 16 KiB 栈再加别的：`csc`/`rcc` 这类要 16 KiB 栈的
  程序在其它板上可能需要更大窗口（`rpi-pico2-rv32` 就是这么继承的）。
- PMP 只有 7 块可用，复杂窗口要靠梯形策略降级；降级会在启动日志里说明。
- 没有 LED/按键，判活只能看串口。

## 相关文件

- `board/rp2350rv-generic/{platform.c,layout.c,kernel.ld,image_def.S,rp2350rv_memmap.h,pmp_map.h}`
- `board/rp2350-generic/layout.c`、`board_internal.h`、`board_devs.c`：被 `base =` 继承的部分
- `arch/rv32/`：trap 入口、PMP CSR、`mret` 与内核线程的 M 态返回、SMP 启动
- `internal/build/codepage.go`：`rvc = auto` 的逐程序回退
- `internal/build/testdata/pmp_map_harness.c`：PMP 映射的主机侧 harness
- `docs/OPEN-ITEMS.md`：PMP 条目预算、跨条目取指规则的实测记录
