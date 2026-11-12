# RP2350 / Pico 2 / ATK-DNRP2350AM（`-target rp2350` / `rp2350smp`）

Cortex-M33（ARMv8-M Secure）真机目标，也是目前功能最完整的一块板：USB CDC 与
UART0 双控制台、ST7789 屏做控制台镜像、MPU 真正隔离任务、flash 上的 littlefs 可写。

## 硬件与启动链

| 项 | 值 |
| --- | --- |
| CPU | 双 Cortex-M33（ARMv8-M，Secure 态），PLL_SYS 150 MHz；`rp2350smp` 才拉 core 1 |
| SRAM | 520 KiB @ `0x20000000`：前 128 KiB 内核，其后 384 KiB = 24 个 16 KiB 窗口 |
| Flash | 4 MiB QSPI（W25Q32，实测确认），XIP @ `0x10000000` |
| 启动 | ROM 自己配 XIP，扫前 4 KiB 找 **PICOBIN IMAGE_DEF**（`board/rp2350/image_def.S`），从 flash 偏移 0 的向量表取 MSP/PC |
| UF2 | family `0xe48bff59`（ARM Secure EXE），产物 `build/rp2350.uf2` |
| 文件系统 | littlefs @ flash `0x80000`，首挂载从 300 KiB 拉伸到 3.5 MiB |
| 任务窗口 | 16 KiB 粒度，栈 8 KiB、护栏 4 KiB |
| 内存保护 | **有**：PMSAv8 MPU（`TOS_ARM_MPU_V8`）：代码 RO+X、数据+堆 RW、栈 RW、栈下护栏；第一次任务切换时打开 |
| 内核堆 | 60 KiB |

板型是**正点原子 ATK-DNRP2350AM**：1.14 寸 ST7789 屏、TF 卡座、CH343P 串口、
按键、LED；同一套代码也能跑在 Pico 2 上（只是没有板载屏/按键）。

## 构建与烧写

```bash
go run ./cmd/tos build -target rp2350
# -> build/kernel-rp2350.elf, build/ramdisk-rp2350.bin, build/rp2350.uf2

go run ./cmd/tos build -target rp2350smp    # 双核
```

烧写入口是 mask ROM 的 **BOOTSEL 盘**（`2e8a:000f`），没有 UART 下载协议：

```bash
# 按住 BOOTSEL 插 USB（或按住 BOOTSEL 再点 RESET），出现 "RP2350" 盘
python scripts/rp2350/flash.py --smoke          # 拷包 + 等重启 + 按 VID/PID 找 CDC + 抓日志 + smoke
python scripts/rp2350/flash.py --boot-1200 COM15 --smoke   # 当前固件能自己重启进 BOOTSEL 时免按键
python scripts/rp2350/flash.py --boot-via COM9 --smoke     # 板子在跑 MicroPython：让 REPL 进 bootloader
python scripts/rp2350/flash.py -p COM14 --no-flash --smoke # 只看串口，不烧写
python scripts/rp2350/flash.py --list                       # 列串口 / CDC / 可移动盘
```

`--port auto` 靠 VID/PID 找 CDC 口，不写死 COM 号。`--smoke` 在看到 `tos> ` 后
依次跑 `echo / ls / free / uptime / cat /proc/uptime / gpio / reboot`。

## 控制台与板载设备

- **USB CDC**（`2e8a:000a`）：板子自己的 USB 口就是控制台，插上就有 shell。
- **UART0**（GPIO0/1，115200 8N1）：CH343P / USB-TTL，和 USB 同时输出，两路都能输入。
- **ST7789 屏**：内核控制台的镜像（`screen mirror off` 关掉），`/dev/lcd0` 是终端，
  支持换行与 40x16 滚动。
- `/dev/led0`、`/dev/key0`、`/proc/dev`；`screen` 程序在屏上画状态卡。

```sh
cat /proc/dev
echo 1 > /dev/led0
cat /dev/key0        # 1 = 按下
screen
echo hello > /dev/lcd0
```

## 线程实验（kthreads）

分支 `threads-preemptive` 的内核线程在 RP2350 的两个架构上都能构建：

```bash
go run ./cmd/tos build -config configs/rp2350smp-kthreads.conf    # Cortex-M33 x2
go run ./cmd/tos build -config configs/rp2350rvsmp-kthreads.conf  # Hazard3 x2
python scripts/rp2350/flash.py --uf2 build/rp2350smp.uf2 --smoke  # 或 ...rvsmp.uf2
```

```sh
tos> threadbench     # 3 线程 vs 顺序；双核上 ratio 应明显低于 100%
tos> kthreadtest     # 17 项边界检查（join、僵尸槽、槽耗尽、fd 共享、重叠睡眠）
tos> prodcons        # 管道阻塞并发，校验和必须一致
```

线程共享窗口、堆和 fd 表；SMP 下 `brk` 和 fd 表有 per-group 自旋锁，组长退出是
延迟回收（等所有线程离开才释放窗口）。设计与已知边界见 `docs/REAL-THREADS.md`。
ARM 上有一处架构细节：Cortex-M 会把 32 B 硬件异常帧压在初始 sp 下方，所以线程的
启动记录放在堆里，不能放栈顶。

## 移植进度

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| IMAGE_DEF + 向量表 + UF2（`0xe48bff59`） | ✅ 实机 | 构建期校验 MSP、reset Thumb 位、IMAGE_DEF 魔数 |
| 150 MHz PLL_SYS，clk_sys 用 FC0 实测 | ✅ 实机 | UART 分频和 SysTick 都来自实测值，PLL 不锁也能继续跑 |
| USB CDC + UART0 双控制台 | ✅ 实机 | `help` 3 KB、管道输出完整 |
| littlefs on flash（ROM flash API + ramfunc） | ✅ 实机 | `dd` 写 4 KiB、读回、pattern 校验 |
| PMSAv8 MPU 每任务四区域 | ✅ 实机 | `prot rodata/heap/stack/null` 四条全部当场拦下 |
| 双核 SMP（`rp2350smp`） | ✅ 实机 | SIO FIFO 握手、per-core 内核栈、`smpbench` 1.89–2.01x |
| ST7789 屏 / 按键 / LED / `/proc/dev` | ✅ 实机 | 屏是控制台镜像 |
| 线程、邮箱、`poll`、管道、`py`、CoreMark | ✅ | 与其它目标同源 |
| `reboot` | ✅ | AIRCR.SYSRESETREQ，ROM 正常重启 |
| WiFi | — | 板上没有 WiFi 硬件，相关命令默认排除 |

## 已知限制

- 512 KiB 里 128 KiB 固定给内核；屏的 DMA 行缓冲（~3.8 KiB）也在内核 `.bss`。
- littlefs 分区从 `0x80000` 到 flash 末尾，内核必须留在前 512 KiB。
- `rp2350` / `rp2350smp` 的堆仍是 60 KiB（历史上按"exec 同时持有压缩+解压缓冲"定的），
  现在 exec 是流式的，可以再压，但还没有实测数据支撑。
- `prot` 在这里是真保护，别在产线上乱跑。

## 相关文件

- `board/rp2350/{platform.c,layout.c,kernel.ld,arm_memmap.h,image_def.S,board_devs.c}`
- `drivers/gpio/rp2350/`、`drivers/spi/rp2350/`、`drivers/display/st7789/rp2350/`、`drivers/usb/rp2350/`
- `internal/build/uf2.go`：`emitRP2350UF2()`、family / IMAGE_DEF / MSP 校验
- `scripts/rp2350/flash.py`：烧写 + 抓日志 + smoke
- `docs/BOARD-ATK-DNRP2350AM.md`：引脚表与踩坑记录
