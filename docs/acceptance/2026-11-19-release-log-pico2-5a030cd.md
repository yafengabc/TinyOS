# 发布镜像精简启动日志（2026-11-19）

```text
commit:            5a030cd
change:            新增 target 无关的 configs/release.conf（boot_trace = 0），并把
                   [smp]/[mpu]/首 tick 自检/starting 等最后的启动噪声并入同一开关；
                   CI 发布矩阵改用该配置；附带 bootsel_poke.py --dtr
baseline:          0193e00
target/config:     rpi-pico2-smp + configs/release.conf（实测）；
                   全 18 目标默认构建 + 5 个目标 release 构建；QEMU 4 目标
board/port/image:  Pico 2 克隆板（RP2350A，8 MiB W25Q64）；UART0 → CH343 COM19
toolchain:         go 1.27.3, clang 23.1.4, python 3.14.0
host:              Windows NT 10.0.17763 (MSYS2 UCRT64)
T0:                PASS（gofmt / go vet ./... / go test ./... -count=1）
T1:                PASS（go run ./cmd/tos build all：18/18；另 5 目标 -config configs/release.conf）
T2:                PASS（tostest smoke rv32 / rv32smp / arm / x86：4× SMOKE TEST OK）
T3:                PASS（release 镜像 UART0：heartbeat 6/6；CDC：Ctrl-D 进 BOOTSEL、shell 可用）
T4:                NOT RUN
logs:              用户终端 + 本地串口采集
```

## 改了什么

| 位置 | 原来 | 现在 |
|---|---|---|
| `kernel/cpu.c` | `[smp] N of M CPU(s) online` 无条件打印 | `#if TOS_BOOT_TRACE` |
| `kernel/main.c` | 首 tick 自检 + `starting /bin/msh ...` 无条件打印 | `#if TOS_BOOT_TRACE`（`plat_irq_enable()` 仍无条件执行） |
| `arch/arm/platform.c` | `[mpu] type=... regions=` 无条件打印（一次性） | `#if TOS_DIAG`；"保护被降级"的告警仍无条件 |
| `configs/release.conf` | — | 新增：只写 `boot_trace = 0`，不写 target |
| `.github/workflows/images.yml` | 发布矩阵 `tos build -target X` | `tos build -target X -config configs/release.conf`；QEMU smoke 保持默认 |

`TinyOS shell (pid 1).` **保留**：`cmd/tostest/emutest.go` 以它（和 `tos> `）判断
shell 起来了。banner 与 heap 行保留。

## release 镜像实际启动日志（UART0，115200）

```text
TinyOS - a tiny multi-platform operating system
user-mode ELF loader | littlefs | PIE
kernel heap: 60 KiB arena

TinyOS shell (pid 1).
tos>
```

同一镜像默认构建（traces on）仍输出全部 `[A]..[E]`、`[P1]/[P2]`、`[smp]`、
`tick self-check`、`[cpu1] secondary ready`、`[mpu] type=...`，`accept33.py` 不受影响
（构建期字符串核对：默认镜像全 PRESENT，release 镜像全 absent）。

## 真机

```text
python scripts/rp2350/heartbeat.py -p COM19 --target rpi-pico2
PASS 1..6
HEARTBEAT OK 6/6
```

## 没做（NOT RUN）

- **ATK 真机 `NOT RUN`**：只构建（M33 SMP、RV32 SMP 均 `-config configs/release.conf`
  构建通过）。开关是共享代码，接板后补一次心跳即可。
- **RP2040 真机 `NOT RUN`**：只构建。
- T4 长跑。
- USB CDC 上"连续第二次长输出被截断"仍是
  [`docs/OPEN-ITEMS.md`](../OPEN-ITEMS.md) 第 5 节记录的既有问题；默认镜像同样复现，
  与本改动无关（release 镜像的 CDC 命令均正常，只是 `ls` 连跑第二次会断）。
