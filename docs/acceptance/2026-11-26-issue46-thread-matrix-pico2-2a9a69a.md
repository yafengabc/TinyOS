# 本机验收：#46 单核 CDC 长输出、§22 M33 线程矩阵、§24 复核（2026-11-26）

```text
commit:            2a9a69a
change:            本机验收三件事：issue #46（Pico 2 单核 ARM 的 CDC 长输出截断）、
                   OPEN-ITEMS §22（M33 用户线程矩阵）、OPEN-ITEMS §24（rv32 wheap）
baseline:          b63eca2
target/config:     rpi-pico2（默认配置，kthreads + usb_pump）；rv32 QEMU 默认配置
board/port/image:  Pico 2；刷机走 UART0 COM19，所有验收走原生 CDC COM27（DTR=True）
toolchain:         go 1.27.3；clang 23.1.4；python 3.14.9
host:              Windows / 本机
T0:                NOT RUN（本轮无代码改动，只验收）
T1:                rpi-pico2 / rpi-pico2-smp / rpi-pico2-rv32-smp 构建通过
T2:                rv32 `wheap` 正常；`soak.py --cycles 3 --targets rv32` 3/3 PASS（1:27）
T3:                CDC 上心跳 6/6；§22 线程矩阵全过（见下）
T4:                NOT RUN
logs:              build/testlogs/issue46-full.log、build/testlogs/soak-rv32-current.log
                   （本地，不入库）
```

## #46：单核 ARM 的 CDC 长输出

#46 的机制是"单核上没有上下文抽 USB TX 环 → `putc()` 在 128+64 B 之后丢字节"，所以
必须在 CDC 上验，UART0 不能代替。内核线程 pump 已是默认（`c33d897`），本轮用
`build/tinyos-rpi-pico2.uf2` 重刷后从 COM27 读（DTR=True）：

| 判据 | 结果 |
|---|---|
| `ls /bin` | 47 names，最后 `wheap`，末尾回到 `tos>` |
| `ls /bin \| wc` | `5 47 328`（328 B > 原先截断的 ~192 B，逐字到达） |
| `crash` | 完整 fault 现场（`cfsr/hfsr/bfar/sfsr/aircr/shcsr`），回到 `tos>` |
| 心跳六项 | 6/6 PASS（启动、时间推进、fs 读、用户程序、fs 写回、fault 隔离） |
| `/proc/ktask` | `0 4096 248 248 usbpump` |

`flash_heartbeat.py` 硬编码 `dtr = False`，在 BOOTSEL 期间 CDC 端口会消失，所以它
不能用于 CDC 镜像：改动只在脚本里加 `--dtr` 也不够（打开端口发生在刷机前）。
本轮因此用 `build/issue46_full.py` 直接在已运行的板子上按同一六项判据验收。

## §22：M33 用户线程矩阵

同一个单核 ARM 镜像，CDC：

| 项 | 结果 |
|---|---|
| `kthreadtest` | `all ok (0 failure(s))` |
| 线程栈溢出 | `ok create an overflowing thread`、`thread stack overflow ... killing`、`ok ... killed only the thread`、`leader survived` |
| `threadbench` | `ratio 104%`（100% = 无开销） |

M33 SMP 的同一矩阵先前已跑（`09da1cd` 卡：泵 4096/248、三退出探针、smpbench 2.00x）。
RV32 两项也已在 `c33d897` 卡里跑过。§22 剩下的只有 RP2040（要物理 BOOTSEL，
OPEN-ITEMS #12 单独安排）。

## §24：本机复核，不复现

远端 `70e8ea1` 说在同一份 `syscall.c` 上不复现，本机确认：

```text
tos> wheap
wheap: 12 KiB declared, 12 KiB taken, pattern intact
tos>
python scripts/soak.py --cycles 3 --targets rv32
rv32     PASS  tasks      20054016     19584  (3 cycles)
1/1 targets passed
```

之前的 A/B 记录在本机也稳定过，但那次 soak 耗时 **8:48**，本轮串行跑只用 **1:27**。
根因是时序而不是代码：被测镜像是 `opt -O2 (kernel) / -Oz (size-opt programs)`，
QEMU TCG 解释执行 -Oz 的用户程序本来就慢，而 `soak.py` 每命令只给 15 s
（`--commandwait-ms` 默认 15000）；当时又并行跑着板测的 dd/heartbeat/reboot，`wheap`
越过窗口后，后面 27 条命令被串进同一段输出，整轮因此判失败。这是**测试超时假象**，
不是内核 bug，也不是 `a004448` 的回归。当前树上不复现，§24 按"未复现"结案。

以后在负载高的机器上跑 `soak.py`，先加大 `--commandwait-ms`（例如 60000）再下结论；
单看 `SMOKE TEST OK` 也不够——那次失败里它也出现了，实际有 27 条命令没回提示符。
