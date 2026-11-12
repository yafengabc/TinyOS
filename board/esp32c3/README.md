# ESP32-C3（`-target esp32c3`）

第一块真机目标：RV32IMC、单核、400 KiB SRAM、4 MiB SPI flash，代码 XIP 在 flash
里执行，文件系统也在 flash 上。板子通过 CH343 串口和板载 USB-Serial/JTAG 两条线
都能进 shell。

## 硬件与启动链

| 项 | 值 |
| --- | --- |
| CPU | RV32IMC，单核，M 模式起步 → U-mode 跑任务 |
| SRAM | 可用 `0x40380000..0x403DC710`（378 KiB），指令别名窗口 |
| Flash | 4 MiB；内核 XIP 从 `0x10000` 起，`0x80000` 起是 littlefs |
| 文件系统 | 3.5 MiB 分区；随镜像发布的只有 300 KiB，首挂载 `lfs_fs_grow` 扩到全分区 |
| 控制台 | UART0（CH343，GPIO20/21，115200）**和** 硬件 USB-Serial/JTAG CDC，同时输出、两路都能输入 |
| 定时器 | TIMG0 → INTMTX；启动时关掉 ROM 留下的 TG0 看门狗 |
| 任务窗口 | 16 KiB 粒度，默认栈 8 KiB、栈下护栏 4 KiB |
| 外设 | SPI2（SCK=GPIO4、MOSI=GPIO5、MISO=GPIO2）、I2C0（SDA=GPIO8、SCL=GPIO9）、LEDC PWM 6 通道（GPIO0..GPIO10） |
| 内核堆 | 80 KiB（`heap=` 可覆盖） |
| 进程表 | 8 槽（`max_procs=` 可覆盖） |

## 构建与烧写

```bash
go run ./cmd/tos build -target esp32c3
# -> build/kernel-esp32c3.elf
# -> build/esp32c3.bin          (烧到 flash 0x0)
# -> build/ramdisk-esp32c3.bin  (烧到 flash 0x80000)
```

用脚本一条龙（自动进下载模式、烧完自己复位并抓 banner）：

```bash
python scripts/esp-c3/flash.py                 # 假定已经 build
python scripts/esp-c3/flash.py --build         # 先 build 再烧
python scripts/esp-c3/flash.py --keep-fs -p COM4   # 只更新内核，保留文件系统
```

手工用 esptool 时就是 build 输出里提示的两条：

```bash
esptool.py write-flash 0x0     build/esp32c3.bin
esptool.py write-flash 0x80000 build/ramdisk-esp32c3.bin
```

CH343 的 DTR/RTS 接了自动下载电路，不需要手按 BOOT。`-target esp32c3` 没有 QEMU
模型：功能回归走 `rv32emu-c`（`tostest emu` / `tostest emutest`），它模拟同一套
RV32IMC。

## 配置

```bash
go run ./cmd/tos build -config configs/esp32c3.conf            # 内核 + shell，不带 WiFi
go run ./cmd/tos build -config configs/esp32c3-wifi.conf       # WiFi + DHCP/ICMP + 全部命令
go run ./cmd/tos build -config configs/esp32c3-wifi-nonet.conf # 只关联 AP
go run ./cmd/tos build -config configs/esp32c3-wifi-softmin.conf # 饱和式软浮点，再小 ~580 B
```

`[user] exclude` 里的程序需要 `/dev/wifi` 或 `/dev/net`，非 WiFi 配置会把它们排除。

## WiFi 开/关的资源对比

同一块 C3 上 `configs/esp32c3.conf`（关）与 `configs/esp32c3-wifi.conf`（开）：

| 指标 | WiFi 关 | WiFi 开 |
| --- | ---: | ---: |
| 内核 flash（`.text`+`.rodata`） | 75.6 KiB | 357.9 KiB |
| 内核 RAM（`.data`+`.bss`） | 121.3 KiB | 187.2 KiB |
| 其中内核堆 arena（`heap=`） | 80 KiB | 80 KiB |
| 内核 RAM 结束地址 | `0x4039e520` | `0x403aece0` |
| 任务区（16 KiB 对齐到 `0x403DC000`） | 240 KiB | 176 KiB |
| 16 KiB 窗口数 | 15 | 11 |
| 32 KiB 窗口数（`msh`/`edit`） | 7 | 5 |
| 128 KiB 窗口（`py`） | 1（余 112 KiB） | 1（余 48 KiB） |
| `esp32c3.bin` 烧写镜像 | 139.6 KiB | 421.9 KiB |

- **内核堆两边都是 80 KiB**（target 默认，`[build] heap=` 可覆盖）。WiFi 吃掉的是
  它自己的 `.data`/`.bss`（约 +66 KiB），把任务区从 240 KiB 压到 176 KiB。
- flash 的大头是 WiFi blob（XIP 常驻）。WiFi 开后内核结束在 `0x7B6A8`，离 littlefs
  的 `0x80000` 只剩约 21 KiB——再加驱动就要先腾空间。
- 窗口按 16 KiB 粒度分配，默认栈 4 KiB、栈下护栏 4 KiB。`py` 的 128 KiB 窗口两边
  都放得下，但 WiFi 开后只剩 48 KiB 给别的任务同时跑。

**内核堆可以降**：无 WiFi 时 `heap = 16384` 实测启动后峰值 768 B，跑完
`py pytest.py`（54 项）加 320 KiB 的 `dd` 写读后峰值 1.5 KiB；任务区因此从
240 KiB 增到 **304 KiB**（15 → 19 个窗口），内核 RAM 降到 57 KiB。

同时能跑多少任务受 `max_procs` 限制，不是窗口数：C3 默认 8 槽，即使有 19 个窗口
也只能同时存在 8 个任务。`configs/esp32c3-16k.conf` 里把它提到 16（多 6.8 KiB
`.bss`），实测 **12 个后台 `sleep` 任务 + shell** 同时在线；任务区仍是 304 KiB，
但内核结束地址刚好落在 16 KiB 边界下方 32 B——内核再长一点就会少一个窗口。

WiFi 构建不要照做。

## 外设与用户命令

三个外设默认编译进镜像（menuconfig 里关掉任意一个，驱动和命令一起消失）：

```sh
cat /proc/dev                # 整张设备表：引脚、总线号、接线
pwm 4 1000 50                # GPIO4：1 kHz、50% 占空比（LEDC 通道 4%6=4）
pwm 4 off                    # 停止，引脚交回 GPIO
spi send 0 10 1000000 HELLO  # SPI2：CS=GPIO10、1 MHz，发一行并打印收到的字节
spi loop 0 10                # 把 MOSI 和 MISO 短接做回环自检
i2c scan 0                   # I2C0：扫 0x08..0x77
i2c read 0 0x3c 0x00 4       # 写寄存器 0x00，再读 4 字节
i2c write 0 0x3c 0x00 0xff   # 写寄存器 0x00 = 0xff
```

- **PWM**：硬件 LEDC，6 个通道 / 4 个定时器，`通道 = gpio % 6`，同定时器的通道共享频率。
  只允许 GPIO0..10（11..17 是 flash，18/19 是 USB，20/21 是控制台）。
- **SPI**：硬件 SPI2，CPU 缓冲模式，每次 `SPI_USR` 最多 64 字节，长传输自动分块并保持 CS。
  CS 是普通 GPIO（默认建议 10），硬件 CS0 关闭；`spi loopback`（PL022 内部回环）在 C3 上不可用。
- **I2C**：**软件位翻转**（开漏 + 内部上拉），100–400 kHz。硬件 I2C0 控制器的命令 FIFO
  （op_code/ack/byte_num）还没接，`i2c` 命令和 `plat_i2c_xfer()` 的语义与 RP2040 完全一致，
  以后换硬件控制器不影响用户程序。

## 线程实验（kthreads）

分支 `threads-preemptive` 在 C3 上做内核线程原型（评估材料见
`docs/REAL-THREADS.md`）。构建与运行：

```bash
go run ./cmd/tos build -config configs/c3-kthreads.conf
python scripts/esp-c3/flash.py
```

```sh
tos> threadbench
threadbench: thread 1 done (sum ...)
threadbench: 3 threads, 300000 iters each: <ms>
threadbench: same work inline:        <ms>
threadbench: sleep() in main, sibling kept running
```

边界情况用 `kthreadtest`（17 项检查：槽耗尽、重复 join、僵尸槽、组长带活线程退出、
阻塞重叠、fd 共享）：

```sh
tos> kthreadtest
tos> kthreadtest leave    # 之后 ps 不应再看到那个线程
```

阻塞并发的验收是 `prodcons`（一个线程读管道、主线程写，互相唤醒）：

```sh
tos> prodcons
prodcons: 32768 bytes through a 512-byte pipe in 25 ms
```

线程由内核调度：tick 可以抢占，一个线程 `read()/sleep()` 只挂起它自己；它们共享
进程的窗口和堆（栈由 `kthread_create()` 用 `sbrk` 从堆里切）。C3 是单核，所以
`threadbench` 的期望结果是**总时间与单线程串行相近**（ratio ≈ 100%），但三个线程
都完成、`join` 正常回收、阻塞按线程生效——这三点才是"真线程"与协作线程的分界。
fd 表是进程级共享的（一个线程 `open/close` 对全组可见），信号/`^C` 也按组生效；
`exec` 在线程里调用会失败。

C3 实测（160 MHz）：`threadbench` 三段同样 90 万次迭代——3 线程 35 ms、单线程
34 ms、caller 顺序 35 ms，ratio 100%；38 次真切换 × 12.6 µs ≈ 0.48 ms（1.4%）。
早先报的 171% 是基准程序的编译器差异（inline 用编译期常量上界被展开），不是
线程开销；`swcost`：`getpid` 3.3 µs、`yield` 12.6 µs。见 `docs/REAL-THREADS.md` §4.5。

## 移植进度

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| XIP 内核、MMU 64 KiB 映射、`.data/.bss` | ✅ 实机 | kernel-xip.ld，`XipFlashBase = 0x10000` |
| 双控制台（UART0 + USB-Serial/JTAG） | ✅ 实机 | 输出镜像到两路，输入取先到的 |
| littlefs on SPI flash（ROM 读 + SPI1 擦写） | ✅ 实机 | `dd` 写入/读回校验通过 |
| PMP 分段、U-mode、`ecall` | ✅ | 与 QEMU rv32 同一套 |
| WiFi（PHY/WPA2/小型 IPv4）+ `ping`/`http` | ✅ 实机 | `drivers/wifi/esp32c3/`，预编译 blob |
| `py` 解释器、管道、线程、邮箱、内核裁剪 | ✅ 实机 | 百行脚本可跑 |
| PWM / SPI2 / I2C0 | ◐ 已实现，待实机验收 | LEDC 与 SPI2 是硬件外设，I2C 是软件位翻转；`pwm`/`spi`/`i2c` 命令已进镜像 |
| 软浮点 / Q16.16 | ✅ | 链接期按需保留 |
| SMP | — | C3 是单核 |

已实测过的数字（见 `docs/OPEN-ITEMS.md`）：内核裁剪全关 text −18.7 KiB、bss −4 KiB；
`proc_t` 856 B；邮箱开启 text +468 B、bss +1,088 B。

## 已知限制

- 用户区约 370 KiB，16 KiB 一个窗口：`sh`/`edit` 各 32 KiB，`py` 要 128 KiB。
- 内核与 WiFi 代码必须在 `0x10000..0x80000`（448 KiB）之内。
- WiFi blob 只针对 C3；`-target rv32` 不带。
- SPI 每次 `SPI_USR` 最多 64 字节，长传输由驱动分块；`spi loopback` 的内部回环在 C3 上不可用。
- I2C 是软件位翻转，实际速率受上拉强度限制；GPIO9 同时是 BOOT 拉绳脚，按住 BOOT 会拉低 SCL。
- PWM/SPI/I2C 与 ADC 共用 GPIO0..5，同一引脚不能同时当两种功能用。

## 相关文件

- `board/esp32c3/platform.c`：UART0/TIMG0/INTMTX/看门狗、SPI flash、MMU
- `board/esp32c3/{pwm.c,spi.c,i2c.c}`：LEDC / SPI2 主机 / 软件 I2C，以及 `c3_apb_hz()` 的分频
- `board/esp32c3/{kernel-xip.ld,kernel.ld,layout.c}`
- `drivers/serial/esp32c3/`：USB-Serial/JTAG
- `drivers/wifi/esp32c3/`：WiFi/PHY/WPA2/IPv4 与 blob
- `scripts/esp-c3/`：真机烧写、验收与寄存器探针
